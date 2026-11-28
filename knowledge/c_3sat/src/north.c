/* North: Knots-based solver with direct/indirect pools and max-dive backjump. */
#include "sat.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { uint8_t mask; uint8_t dead; uint32_t bad; } Varnt;
typedef struct { size_t clause; size_t vi; size_t *chg; size_t cn, cap; } Dec;
typedef struct {
    size_t *vars; size_t nv, cv; size_t *cls; size_t nc, cc;
    uint64_t sig;   /* sorted vars signature hash for mono block */
    bool gray;
} Pool;
typedef struct {
    const Cnf *cnf; size_t nvrs, ncls;
    int8_t *val;
    size_t *owner;      /* var -> clause that set it (internal to that clause/pool) */
    size_t *var_pool;   /* var -> pool id */
    Pool *pools; size_t np, cp;
    Varnt **vs; size_t *vn;
    Dec *stk; size_t sp, sc;
    size_t *order;      /* clause processing order, grouped by pool */
    size_t *pos;        /* clause -> index in order */
    const bool *ref;    /* optional reference SAT assignment for tracing */
    int trace;
    bool random_mode;
    uint64_t rng;
    KnotsSort sort;
    bool mono;
    bool reverse;
    size_t keep_bad;
    bool gray_phase; /* reprocessing gray blocks */
    bool north;
    size_t max_dive;
} K;

static bool ltrue(const Lit*l,int8_t v){return v>=0 && ((v==1)==l->positive);}
static bool lfalse(const Lit*l,int8_t v){return v>=0 && ((v==1)!=l->positive);}

static bool pool_has_var(Pool*p,size_t v){ for(size_t i=0;i<p->nv;i++) if(p->vars[i]==v) return true; return false; }
static bool pool_add_var(Pool*p,size_t v){
    if(pool_has_var(p,v)) return true;
    if(p->nv==p->cv){ size_t c=p->cv?p->cv*2:4; size_t*n=realloc(p->vars,c*sizeof*n); if(!n) return false; p->vars=n; p->cv=c; }
    p->vars[p->nv++]=v; return true;
}
static bool pool_add_cls(Pool*p,size_t ci){
    if(p->nc==p->cc){ size_t c=p->cc?p->cc*2:4; size_t*n=realloc(p->cls,c*sizeof*n); if(!n) return false; p->cls=n; p->cc=c; }
    p->cls[p->nc++]=ci; return true;
}
static bool new_pool(K*k,size_t *out){
    if(k->np==k->cp){ size_t c=k->cp?k->cp*2:16; Pool*np=realloc(k->pools,c*sizeof*np); if(!np) return false; memset(np+k->cp,0,(c-k->cp)*sizeof*np); k->pools=np; k->cp=c; }
    *out=k->np++; Pool*p=&k->pools[*out]; memset(p,0,sizeof*p); return true;
}
static void merge_pool(K*k,size_t dst,size_t src){
    if(dst==src) return;
    Pool*d=&k->pools[dst], *s=&k->pools[src];
    for(size_t i=0;i<s->nv;i++){ pool_add_var(d,s->vars[i]); k->var_pool[s->vars[i]]=dst; }
    for(size_t i=0;i<s->nc;i++) pool_add_cls(d,s->cls[i]);
    s->nv=0; s->nc=0;
}
static bool attach_pool(K*k,size_t ci){
    const Lit*cl=cnf_clause(k->cnf,ci); size_t n=cnf_clause_len(k->cnf,ci);
    size_t dst=SIZE_MAX;
    for(size_t i=0;i<n;i++){
        size_t p=k->var_pool[cl[i].var];
        if(p==SIZE_MAX) continue;
        if(dst==SIZE_MAX) dst=p;
        else if(dst!=p) merge_pool(k,dst,p);
    }
    if(dst==SIZE_MAX){ if(!new_pool(k,&dst)) return false; }
    for(size_t i=0;i<n;i++){
        size_t v=cl[i].var;
        if(k->var_pool[v]==SIZE_MAX){ k->var_pool[v]=dst; if(!pool_add_var(&k->pools[dst],v)) return false; }
        else if(k->var_pool[v]!=dst) merge_pool(k,dst,k->var_pool[v]);
    }
    if(!pool_add_cls(&k->pools[dst],ci)) return false;
    /* after merges, ensure all clause vars in dst */
    for(size_t i=0;i<n;i++) k->var_pool[cl[i].var]=dst;
    return true;
}

static void build_variants(K*k,size_t ci){
    size_t n=cnf_clause_len(k->cnf,ci);
    size_t mmax=(n>=8)?256:(1u<<n);
    Varnt *vs=malloc((mmax?mmax:1)*sizeof*vs); size_t vn=0;
    if(!vs){k->vs[ci]=NULL;k->vn[ci]=0;return;}
    for(size_t m=1;m<mmax;m++){ vs[vn].mask=(uint8_t)m; vs[vn].dead=0; vs[vn].bad=0; vn++; }
    k->vs[ci]=vs; k->vn[ci]=vn;
}

static bool clause_sat(K*k,size_t ci){
    const Lit*cl=cnf_clause(k->cnf,ci); size_t n=cnf_clause_len(k->cnf,ci);
    for(size_t i=0;i<n;i++) if(ltrue(&cl[i],k->val[cl[i].var])) return true;
    return false;
}
static bool clause_conflict(K*k,size_t ci){
    const Lit*cl=cnf_clause(k->cnf,ci); size_t n=cnf_clause_len(k->cnf,ci);
    for(size_t i=0;i<n;i++) if(!lfalse(&cl[i],k->val[cl[i].var])) return false;
    return true;
}

static bool push_dec(K*k,size_t clause,size_t vi){
    if(k->sp==k->sc){ size_t c=k->sc?k->sc*2:32; Dec*ns=realloc(k->stk,c*sizeof*ns); if(!ns) return false;
        for(size_t i=k->sc;i<c;i++){ ns[i].chg=malloc(8*sizeof*ns[i].chg); ns[i].cn=0; ns[i].cap=ns[i].chg?8:0; }
        k->stk=ns; k->sc=c;
    }
    Dec*d=&k->stk[k->sp++]; d->clause=clause; d->vi=vi; d->cn=0; return true;
}
static bool dec_add(Dec*d,size_t var){
    if(d->cn>=d->cap) return false; /* fixed 3-var clause max, 8 slots */
    d->chg[d->cn++]=var; return true;
}
static size_t canon_pool(K*k,size_t p){
    /* pools may be stale after merges; var_pool is canonical for vars present */
    if(p==SIZE_MAX) return SIZE_MAX;
    return p;
}
static size_t clause_pool(K*k,size_t ci){
    const Lit*cl=cnf_clause(k->cnf,ci); size_t n=cnf_clause_len(k->cnf,ci);
    for(size_t i=0;i<n;i++){ size_t p=k->var_pool[cl[i].var]; if(p!=SIZE_MAX) return p; }
    return SIZE_MAX;
}
static inline int lit_want(const K*k,bool positive,unsigned bit){ int w=bit?(positive?1:0):(positive?0:1); return k->reverse?!w:w; }
static void clear_dead_after(K*k,size_t jcl){
    /* clear dead marks for clauses after jcl in same pool only; other pools independent */
    size_t pid = clause_pool(k,jcl);
    size_t jpos = k->pos[jcl];
    for(size_t p=jpos+1;p<k->ncls;p++){
        size_t i=k->order[p];
        if(pid!=SIZE_MAX && clause_pool(k,i)!=pid) continue;
        for(size_t vi=0;vi<k->vn[i];vi++){ k->vs[i][vi].dead=0; k->vs[i][vi].bad=0; }
    }
}

static void undo_dec(K*k,Dec*d){
    for(size_t i=0;i<d->cn;i++){ size_t v=d->chg[i]; if(k->owner[v]==d->clause) k->owner[v]=SIZE_MAX; k->val[v]=-1; }
    d->cn=0;
}

static int apply_variant(K*k,size_t ci,size_t vi,Dec*d){
    const Lit*cl=cnf_clause(k->cnf,ci); size_t n=cnf_clause_len(k->cnf,ci); uint8_t m=k->vs[ci][vi].mask;
    for(size_t i=0;i<n;i++){ int want=lit_want(k,cl[i].positive,(m>>i)&1u); int8_t cur=k->val[cl[i].var]; if(cur>=0 && cur!=want) return 0; }
    for(size_t i=0;i<n;i++){
        int want=lit_want(k,cl[i].positive,(m>>i)&1u);
        if(k->val[cl[i].var]<0){
            k->val[cl[i].var]=(int8_t)want;
            if(k->owner[cl[i].var]==SIZE_MAX) k->owner[cl[i].var]=ci;
            if(!dec_add(d,cl[i].var)) return -1;
        }
    }
    return 1;
}

static size_t owner_dec(K*k,size_t var){
    size_t oc=k->owner[var]; if(oc==SIZE_MAX) return SIZE_MAX;
    for(size_t i=k->sp;i>0;i--) if(k->stk[i-1].clause==oc) return i-1;
    return SIZE_MAX;
}
static bool any_live(K*k,size_t ci){ for(size_t vi=0;vi<k->vn[ci];vi++) if(!k->vs[ci][vi].dead) return true; return false; }

static bool try_restate_at(K*k,size_t jump){
    Dec*jd=&k->stk[jump];
    size_t jcl=jd->clause; size_t old_vi=jd->vi;
    static size_t tr=0; ++tr; if(tr<20) fprintf(stderr,"NORTH try_restate_at tr=%zu jump=%zu jcl=%zu sp=%zu\n",tr,jump,jcl,k->sp);
    /* Restate: undo to this decision and try next variants. 
       Don't permanently kill old_vi here - it may be valid in different external context. */
    while(k->sp>jump){ k->sp--; undo_dec(k,&k->stk[k->sp]); }
    clear_dead_after(k,jcl);
    for(size_t vi=old_vi+1;vi<k->vn[jcl];vi++){
        if(k->vs[jcl][vi].dead) continue;
        if(!push_dec(k,jcl,vi)) return false;
        Dec*d=&k->stk[k->sp-1]; int ar=apply_variant(k,jcl,vi,d);
        if(ar<0) return false;
        if(ar==0){ k->sp--; undo_dec(k,d); continue; }
        if(k->trace && k->ref){
            /* check if chosen variant contradicts ref on any set var */
            const Lit*cl=cnf_clause(k->cnf,jcl); size_t n=cnf_clause_len(k->cnf,jcl); uint8_t m=k->vs[jcl][vi].mask;
            bool bad=false; for(size_t i=0;i<n;i++){ int want=lit_want(k,cl[i].positive,(m>>i)&1u); bool rv=k->ref[cl[i].var]; if((want?1:0)!=(rv?1:0)) bad=true; }
            if(bad) fprintf(stderr,"TRACE restate jcl=%zu vi=%zu deviates-from-ref\n",jcl,vi);
        }
        return true;
    }
    /* exhausted in this context; before gray processing, don't drain pool */
    if(!k->gray_phase) {
        if(push_dec(k,jcl,old_vi)){
            Dec*d=&k->stk[k->sp-1]; int ar=apply_variant(k,jcl,old_vi,d);
            if(ar<=0){ k->sp--; undo_dec(k,d); }
        }
        return false;
    }
    k->vs[jcl][old_vi].dead=1;
    for(size_t vi=0;vi<k->vn[jcl];vi++){
        if(k->vs[jcl][vi].dead) continue;
        if(!push_dec(k,jcl,vi)) return false;
        Dec*d=&k->stk[k->sp-1]; int ar=apply_variant(k,jcl,vi,d);
        if(ar<0) return false;
        if(ar==0){ k->sp--; undo_dec(k,d); continue; }
        return true;
    }
    if(push_dec(k,jcl,old_vi)){
        Dec*d=&k->stk[k->sp-1]; int ar=apply_variant(k,jcl,old_vi,d);
        if(ar<=0){ k->sp--; undo_dec(k,d); }
    }
    return false;
}

static bool same_varset_clause_pool(K*k,size_t ci,size_t pid){
    if(pid==SIZE_MAX) return false;
    const Lit*cl=cnf_clause(k->cnf,ci); size_t n=cnf_clause_len(k->cnf,ci);
    Pool*p=&k->pools[pid];
    if(p->nv!=n) return false;
    for(size_t i=0;i<n;i++) if(!pool_has_var(p,cl[i].var)) return false;
    return true;
}
static bool indirect_pool_for_clause(K*k,size_t ci,size_t pid){
    if(pid==SIZE_MAX) return false;
    const Lit*cl=cnf_clause(k->cnf,ci); size_t n=cnf_clause_len(k->cnf,ci);
    Pool*p=&k->pools[pid];
    bool share=false;
    for(size_t i=0;i<n;i++) if(pool_has_var(p,cl[i].var)){ share=true; break; }
    if(!share) return false;
    return !same_varset_clause_pool(k,ci,pid);
}
static bool pool_has_external_set(K*k,size_t pid){
    if(pid==SIZE_MAX) return false;
    Pool*p=&k->pools[pid];
    for(size_t i=0;i<p->nv;i++){
        size_t v=p->vars[i];
        if(k->val[v]<0) continue;
        size_t oc=k->owner[v];
        if(oc==SIZE_MAX) return true; /* set externally */
        if(clause_pool(k,oc)!=pid) return true;
    }
    return false;
}

static bool try_restate_north(K*k,size_t jump,size_t depth,size_t max_dive,size_t *last_jump){
    Dec*jd=&k->stk[jump];
    size_t jcl=jd->clause; size_t old_vi=jd->vi;
    while(k->sp>jump){ k->sp--; undo_dec(k,&k->stk[k->sp]); }
    clear_dead_after(k,jcl);
    for(size_t vi=old_vi+1;vi<k->vn[jcl];vi++){
        if(k->vs[jcl][vi].dead) continue;
        if(!push_dec(k,jcl,vi)) return false;
        Dec*d=&k->stk[k->sp-1]; int ar=apply_variant(k,jcl,vi,d);
        if(ar<0) return false;
        if(ar==0){ k->sp--; undo_dec(k,d); continue; }
        if(last_jump) *last_jump=jump;
        return true;
    }
    /* at max dive, last pool in chain kills bad variant */
    if(depth >= max_dive){
        k->vs[jcl][old_vi].dead=1;
        k->vs[jcl][old_vi].bad++;
        for(size_t vi=0;vi<k->vn[jcl];vi++){
            if(k->vs[jcl][vi].dead) continue;
            if(!push_dec(k,jcl,vi)) return false;
            Dec*d=&k->stk[k->sp-1]; int ar=apply_variant(k,jcl,vi,d);
            if(ar<0) return false;
            if(ar==0){ k->sp--; undo_dec(k,d); continue; }
            if(last_jump) *last_jump=jump;
            return true;
        }
    } else {
        /* keep bad variant, just backtrack; try reapply old as fallback to preserve state */
        if(push_dec(k,jcl,old_vi)){
            Dec*d=&k->stk[k->sp-1]; int ar=apply_variant(k,jcl,old_vi,d);
            if(ar<=0){ k->sp--; undo_dec(k,d); }
        }
    }
    return false;
}

static bool restate_external(K*k,size_t ci){
    static size_t rc=0; ++rc; if(rc<20 || rc % 50000 == 0) fprintf(stderr,"NORTH restate rc=%zu ci=%zu sp=%zu\n",rc,ci,k->sp);
    if(!k->north){
        const Lit*cl=cnf_clause(k->cnf,ci); size_t n=cnf_clause_len(k->cnf,ci);
        size_t pid = clause_pool(k,ci);
        size_t local[8], ext[8]; size_t ln=0,en=0;
        for(size_t i=0;i<n;i++){
            size_t od=owner_dec(k,cl[i].var);
            if(od==SIZE_MAX) continue;
            bool seen=false;
            for(size_t j=0;j<ln;j++) if(local[j]==od) seen=true;
            for(size_t j=0;j<en;j++) if(ext[j]==od) seen=true;
            if(seen) continue;
            size_t oc=k->stk[od].clause;
            bool is_local = (pid!=SIZE_MAX && clause_pool(k,oc)==pid);
            if(is_local){ if(ln<8) local[ln++]=od; } else { if(en<8) ext[en++]=od; }
        }
        for(size_t i=0;i<ln;i++) for(size_t j=i+1;j<ln;j++) if(local[j]>local[i]){size_t t=local[i];local[i]=local[j];local[j]=t;}
        for(size_t i=0;i<en;i++) for(size_t j=i+1;j<en;j++) if(ext[j]>ext[i]){size_t t=ext[i];ext[i]=ext[j];ext[j]=t;}
        for(size_t i=0;i<ln;i++) if(try_restate_at(k,local[i])) return true;
        if(pid!=SIZE_MAX && !pool_has_external_set(k,pid)) return false;
        for(size_t i=0;i<en;i++) if(try_restate_at(k,ext[i])) return true;
        for(size_t i=k->sp;i>0;i--){
            size_t od=i-1; size_t oc=k->stk[od].clause;
            if(pid!=SIZE_MAX && clause_pool(k,oc)!=pid) continue;
            if(try_restate_at(k,od)) return true;
        }
        if(pid!=SIZE_MAX && !pool_has_external_set(k,pid)) return false;
        while(k->sp>0){ if(try_restate_at(k,k->sp-1)) return true; }
        return false;
    }

    /* North: direct pool (same varset) does not drain - backjump to indirect pools.
       Indirect pool shares >=1 var but not same varset. Last pool in chain (depth>=max_dive) removes bad variants. */
    size_t pid = clause_pool(k,ci);
    size_t cur_ci = ci;
    size_t max_dive = k->max_dive;
    if(max_dive > k->np) max_dive = k->np ? k->np : 1;
    for(size_t depth=1; depth<=max_dive && k->sp>0; depth++){
        size_t cand[32]; size_t cn=0;
        { static size_t dc=0; ++dc; if(dc<40) fprintf(stderr,"NORTH depth=%zu cur_ci=%zu sp=%zu\n",depth,cur_ci,k->sp); }
        /* find decision owners related to cur_ci: indirect pools first, then direct */
        const Lit*cl=cnf_clause(k->cnf,cur_ci); size_t n=cnf_clause_len(k->cnf,cur_ci);
        for(size_t i=0;i<n && cn<32;i++){
            size_t od=owner_dec(k,cl[i].var);
            if(od==SIZE_MAX) continue;
            bool seen=false; for(size_t j=0;j<cn;j++) if(cand[j]==od) seen=true;
            if(seen) continue;
            size_t oc=k->stk[od].clause;
            size_t opid=clause_pool(k,oc);
            bool direct = (pid!=SIZE_MAX && opid==pid && same_varset_clause_pool(k,oc,pid));
            bool indirect = (pid!=SIZE_MAX) ? indirect_pool_for_clause(k,oc,pid) : (opid!=pid);
            if(direct || indirect) cand[cn++]=od;
        }
        /* if no var-owner candidates, fall back to any stack decision */
        if(cn==0){ for(size_t i=k->sp;i>0 && cn<32;i--) cand[cn++]=i-1; }
        /* sort desc (newest first), prefer indirect over direct at same level */
        for(size_t i=0;i<cn;i++) for(size_t j=i+1;j<cn;j++){
            size_t oi=cand[i], oj=cand[j];
            size_t ocj=k->stk[oj].clause; size_t opj=clause_pool(k,ocj);
            size_t oci=k->stk[oi].clause; size_t opi=clause_pool(k,oci);
            bool dj = (pid!=SIZE_MAX && opj==pid && same_varset_clause_pool(k,ocj,pid));
            bool di = (pid!=SIZE_MAX && opi==pid && same_varset_clause_pool(k,oci,pid));
            bool swap = (oj>oi);
            if(di!=dj) swap = di && !dj; /* indirect before direct */
            else swap = (oj>oi);
            if(swap){ size_t t=cand[i]; cand[i]=cand[j]; cand[j]=t; }
        }
        for(size_t i=0;i<cn;i++){
            size_t od=cand[i];
            size_t oc=k->stk[od].clause;
            size_t opid=clause_pool(k,oc);
            bool direct = (pid!=SIZE_MAX && opid==pid && same_varset_clause_pool(k,oc,pid));
            /* direct pool: don't drain, just backjump deeper */
            if(direct && depth < k->max_dive){
                size_t lj=SIZE_MAX;
                if(try_restate_north(k,od,depth,max_dive,&lj)) return true;
                cur_ci = oc; /* continue chain from this pool */
                goto next_depth;
            }
            size_t lj=SIZE_MAX;
            if(try_restate_north(k,od,depth,max_dive,&lj)) return true;
            cur_ci = oc;
        }
        next_depth:;
    }
    /* fallback: normal restate to avoid false UNKNOWN where possible */
    { static size_t fc=0; ++fc; if(fc<20) fprintf(stderr,"NORTH fallback sp=%zu\n",k->sp); }
    while(k->sp>0){
        size_t jump=k->sp-1;
        if(try_restate_at(k,jump)) { {static size_t fs=0;++fs;if(fs<20) fprintf(stderr,"NORTH fbok sp=%zu\n",k->sp);} return true; }
        /* try_restate_at may have restored jump; force pop to make progress */
        if(k->sp>jump){ k->sp--; undo_dec(k,&k->stk[k->sp]); }
        else if(k->sp>0){ k->sp--; undo_dec(k,&k->stk[k->sp]); }
    }
    { static size_t ff=0;++ff;if(ff<20) fprintf(stderr,"NORTH fbfail sp=%zu\n",k->sp); }
    return false;
}

static uint64_t krng(K*k){ uint64_t x=k->rng; x^=x<<13; x^=x>>7; x^=x<<17; return k->rng=x; }

static uint64_t vars_sig(const size_t *v, size_t n){
    /* sort copy small n<=3 usually */
    size_t t[8]; size_t m=n<8?n:8;
    for(size_t i=0;i<m;i++) t[i]=v[i];
    for(size_t i=0;i<m;i++) for(size_t j=i+1;j<m;j++) if(t[j]<t[i]){ size_t x=t[i]; t[i]=t[j]; t[j]=x; }
    uint64_t h=1469598103934665603ULL;
    for(size_t i=0;i<m;i++){ h^=(uint64_t)t[i]; h*=1099511628211ULL; }
    h ^= (uint64_t)n<<56;
    return h;
}
static uint64_t clause_sig(const Cnf *cnf, size_t ci){
    const Lit*cl=cnf_clause(cnf,ci); size_t n=cnf_clause_len(cnf,ci);
    size_t v[8]; for(size_t i=0;i<n && i<8;i++) v[i]=cl[i].var;
    return vars_sig(v,n);
}

static void print_clause(FILE *f, const Cnf *cnf, size_t ci){
    const Lit*cl=cnf_clause(cnf,ci); size_t n=cnf_clause_len(cnf,ci);
    fprintf(f, "clause %zu [", ci);
    for(size_t i=0;i<n;i++) fprintf(f, "%s%u%s", cl[i].positive?"":"-", cl[i].var+1, i+1<n?" ":"");
    fprintf(f, "]");
}

static void print_variant(FILE *f, K*k, size_t ci, uint8_t mask, bool dead){
    const Cnf*cnf=k->cnf; const Lit*cl=cnf_clause(cnf,ci); size_t n=cnf_clause_len(cnf,ci);
    fprintf(f, "%s{", dead?"dead":"live");
    for(size_t i=0;i<n;i++){
        int want=lit_want(k,cl[i].positive,(mask>>i)&1u);
        fprintf(f, "%s%u=%d", i?" ":"", cl[i].var+1, want);
    }
    fprintf(f, "}");
}

static void mono_report(K*k, size_t fail_ci){
    if(!k->mono) return;
    size_t fpid = clause_pool(k, fail_ci);
    uint64_t fsig = clause_sig(k->cnf, fail_ci);
    size_t fn = cnf_clause_len(k->cnf, fail_ci);
    fprintf(stderr, "MONO fail %zu pool=%zu vars=%zu sig=%016llx\n", fail_ci, fpid, fn, (unsigned long long)fsig);
    /* report failed block */
    fprintf(stderr, "MONO failed-block vars=");
    { const Lit*cl=cnf_clause(k->cnf,fail_ci); for(size_t i=0;i<fn;i++) fprintf(stderr, "%s%u", i?" ":"", cl[i].var+1); }
    fprintf(stderr, "\n");
    size_t block_len=0; for(size_t ci=0;ci<k->ncls;ci++) if(clause_sig(k->cnf,ci)==fsig) block_len++;
    size_t space = (fn>=8)?0:((size_t)1<<fn);
    size_t avail = space>block_len ? space-block_len : 0;
    fprintf(stderr, "MONO failed-block clauses=%zu space=%zu available=%zu\n", block_len, space, avail);
    fprintf(stderr, "  active:");
    { const Lit*cl=cnf_clause(k->cnf,fail_ci); bool bs=false;
      for(size_t i=0;i<fn;i++){ int8_t vv=k->val[cl[i].var]; fprintf(stderr," %u=%s",cl[i].var+1,vv<0?"?":(vv?"1":"0")); }
      for(size_t ci=0;ci<k->ncls && !bs;ci++) if(clause_sig(k->cnf,ci)==fsig) bs=clause_sat(k,ci);
      fprintf(stderr," block_sat=%s\n", bs?"yes":"no"); }
    if(space && block_len>=space) fprintf(stderr, "MONO failed-block UNSOLVABLE full block\n");
    for(size_t ci=0;ci<k->ncls;ci++) if(clause_sig(k->cnf,ci)==fsig){
        fprintf(stderr, "  "); print_clause(stderr,k->cnf,ci);
        fprintf(stderr, " variants:");
        for(size_t vi=0;vi<k->vn[ci];vi++){ fprintf(stderr," "); print_variant(stderr,k,ci,k->vs[ci][vi].mask,k->vs[ci][vi].dead!=0); }
        fprintf(stderr,"\n");
    }
    /* external owner pool */
    size_t ext_pid = SIZE_MAX;
    if(fpid!=SIZE_MAX){
        Pool *p=&k->pools[fpid];
        for(size_t i=0;i<p->nv;i++){ size_t v=p->vars[i]; size_t o=k->owner[v]; if(o!=SIZE_MAX && clause_pool(k,o)!=fpid){ ext_pid=clause_pool(k,o); break; } }
    }
    if(ext_pid!=SIZE_MAX){
        fprintf(stderr, "MONO external-pool %zu\n", ext_pid);
        /* find representative block in external pool: block of fail_ci? no, block that has external vars */
        /* report all blocks in ext pool */
        for(size_t i=0;i<k->pools[ext_pid].nc;i++){
            size_t ci=k->pools[ext_pid].cls[i]; uint64_t sig=clause_sig(k->cnf,ci);
            /* report block once */
            bool first=true; for(size_t j=0;j<i;j++) if(clause_sig(k->cnf,k->pools[ext_pid].cls[j])==sig){first=false;break;}
            if(!first) continue;
            size_t n=cnf_clause_len(k->cnf,ci); size_t bl=0; for(size_t x=0;x<k->ncls;x++) if(clause_sig(k->cnf,x)==sig) bl++;
            size_t spc=(n>=8)?0:((size_t)1<<n); size_t av=spc>bl?spc-bl:0;
            fprintf(stderr, "  MONO ext-block vars="); const Lit*cl=cnf_clause(k->cnf,ci); for(size_t t=0;t<n;t++) fprintf(stderr,"%s%u",t?" ":"",cl[t].var+1);
            fprintf(stderr, " clauses=%zu space=%zu available=%zu\n", bl,spc,av);
            fprintf(stderr, "    active:");
            bool block_sat=false;
            for(size_t t=0;t<n;t++){ int8_t vv=k->val[cl[t].var]; fprintf(stderr," %u=%s", cl[t].var+1, vv<0?"?":(vv?"1":"0")); }
            for(size_t x=0;x<k->ncls && !block_sat;x++) if(clause_sig(k->cnf,x)==sig) block_sat=clause_sat(k,x);
            fprintf(stderr, " block_sat=%s\n", block_sat?"yes":"no");
            for(size_t x=0;x<k->ncls;x++) if(clause_sig(k->cnf,x)==sig){
                fprintf(stderr,"    "); print_clause(stderr,k->cnf,x);
                fprintf(stderr," variants:");
                for(size_t vi=0;vi<k->vn[x];vi++){ fprintf(stderr," "); print_variant(stderr,k,x,k->vs[x][vi].mask,k->vs[x][vi].dead!=0); }
                fprintf(stderr,"\n");
            }
        }
    } else {
        fprintf(stderr, "MONO external-pool none\n");
    }
}

static void build_order(K*k, KnotsSort sort){
    size_t oi=0; bool *seen=calloc(k->ncls?k->ncls:1,1);
    if(!seen) return;
    if(sort==KNOTS_SORT_SATURATION){
        /* group clauses by mono-signature (same var set), most saturated (largest block) first */
        uint64_t *sigs=malloc((k->ncls?k->ncls:1)*sizeof*sigs);
        size_t *blen=calloc(k->ncls?k->ncls:1,sizeof*blen);
        if(sigs&&blen){
            for(size_t ci=0;ci<k->ncls;ci++) sigs[ci]=clause_sig(k->cnf,ci);
            for(size_t i=0;i<k->ncls;i++) for(size_t j=0;j<k->ncls;j++) if(sigs[j]==sigs[i]) blen[i]++;
            bool any=true;
            while(any){
                any=false; size_t best=SIZE_MAX; size_t best_b=0;
                for(size_t ci=0;ci<k->ncls;ci++){
                    if(seen[ci]) continue;
                    if(!any || blen[ci]>best_b){ any=true; best=ci; best_b=blen[ci]; }
                }
                if(!any) break;
                uint64_t bs=sigs[best];
                for(size_t ci=0;ci<k->ncls;ci++) if(!seen[ci] && sigs[ci]==bs){ k->order[oi++]=ci; seen[ci]=true; }
            }
        }
        free(sigs); free(blen);
    } else if(sort!=KNOTS_SORT_NONE){
        size_t *pop=calloc(k->nvrs?k->nvrs:1,sizeof*pop);
        if(pop){
            for(size_t ci=0;ci<k->ncls;ci++){
                const Lit*cl=cnf_clause(k->cnf,ci); size_t n=cnf_clause_len(k->cnf,ci);
                for(size_t i=0;i<n;i++) pop[cl[i].var]++;
            }
            bool any=true;
            while(any){
                any=false; size_t best_v=SIZE_MAX; size_t best_p=0;
                for(size_t v=0;v<k->nvrs;v++){
                    if(pop[v]==0) continue;
                    bool has=false;
                    for(size_t ci=0;ci<k->ncls && !has;ci++) if(!seen[ci]){ const Lit*cl=cnf_clause(k->cnf,ci); size_t n=cnf_clause_len(k->cnf,ci); for(size_t i=0;i<n;i++) if(cl[i].var==v){has=true;break;} }
                    if(!has){ pop[v]=0; continue; }
                    if(best_v==SIZE_MAX || (sort==KNOTS_SORT_DESC ? pop[v]>best_p : pop[v]<best_p)){
                        best_v=v; best_p=pop[v]; any=true;
                    }
                }
                if(!any) break;
                for(size_t ci=0;ci<k->ncls;ci++){
                    if(seen[ci]) continue;
                    const Lit*cl=cnf_clause(k->cnf,ci); size_t n=cnf_clause_len(k->cnf,ci);
                    bool has=false; for(size_t i=0;i<n;i++) if(cl[i].var==best_v){has=true;break;}
                    if(has){ k->order[oi++]=ci; seen[ci]=true; }
                }
                pop[best_v]=0;
            }
            free(pop);
        }
    }
    if(oi==0){
        /* default: pool-grouped */
        for(size_t i=0;i<k->ncls;i++){
            if(seen[i]) continue;
            size_t pid = clause_pool(k,i);
            if(pid==SIZE_MAX){ k->order[oi++]=i; seen[i]=true; continue; }
            Pool *p=&k->pools[pid];
            for(size_t j=0;j<p->nc;j++){ size_t ci=p->cls[j]; if(!seen[ci]){ k->order[oi++]=ci; seen[ci]=true; } }
            if(!seen[i]){ k->order[oi++]=i; seen[i]=true; }
        }
    }
    for(size_t i=0;i<k->ncls;i++) if(!seen[i]) k->order[oi++]=i;
    for(size_t i=0;i<k->ncls;i++) k->pos[k->order[i]]=i;
    if(sort==KNOTS_SORT_SATURATION && k->ncls>0){
        uint64_t s0=clause_sig(k->cnf,k->order[0]); size_t b0=0; for(size_t i=0;i<k->ncls;i++) if(clause_sig(k->cnf,i)==s0) b0++;
        fprintf(stderr,"SAT-RANK first clause=%zu block_len=%zu\n",k->order[0],b0);
    }
    free(seen);
}
static size_t pick_next(K*k,size_t pi){
    if(k->sort==KNOTS_SORT_DYN_DESC || k->sort==KNOTS_SORT_DYN_ASC){
        /* recalc popularity among unsatisfied clauses from pi onward */
        size_t *pop=calloc(k->nvrs?k->nvrs:1,sizeof*pop);
        if(pop){
            for(size_t j=pi;j<k->ncls;j++){
                size_t ci=k->order[j]; if(clause_sat(k,ci)) continue;
                const Lit*cl=cnf_clause(k->cnf,ci); size_t n=cnf_clause_len(k->cnf,ci);
                for(size_t i=0;i<n;i++) pop[cl[i].var]++;
            }
            size_t best_j=pi; size_t best_p=0; bool have=false;
            for(size_t j=pi;j<k->ncls;j++){
                size_t ci=k->order[j]; if(clause_sat(k,ci)) continue;
                const Lit*cl=cnf_clause(k->cnf,ci); size_t n=cnf_clause_len(k->cnf,ci);
                size_t p=0; for(size_t i=0;i<n;i++){ size_t pv=pop[cl[i].var]; if(pv>p) p=pv; }
                if(!have || (k->sort==KNOTS_SORT_DYN_DESC ? p>best_p : p<best_p)){ have=true; best_p=p; best_j=j; }
            }
            free(pop);
            if(have && best_j!=pi){ size_t tmp=k->order[pi]; k->order[pi]=k->order[best_j]; k->order[best_j]=tmp; k->pos[k->order[pi]]=pi; k->pos[k->order[best_j]]=best_j; }
        }
        return pi;
    }
    if(!k->random_mode) return pi;
    size_t n=k->ncls-pi; if(n<=1) return pi;
    size_t j = pi + (size_t)(krng(k) % n);
    size_t tmp=k->order[pi]; k->order[pi]=k->order[j]; k->order[j]=tmp;
    k->pos[k->order[pi]]=pi; k->pos[k->order[j]]=j;
    return pi;
}

static bool is_gray_clause(K*k,size_t ci){
    size_t pid=clause_pool(k,ci);
    return pid!=SIZE_MAX && k->pools[pid].gray;
}
static void mark_gray(K*k,size_t ci){
    size_t pid=clause_pool(k,ci);
    if(pid!=SIZE_MAX) k->pools[pid].gray=true;
}

static bool solve(K*k){
    size_t pi=0; size_t it=0;
    while(1){
        ++it; if(it<30 || it % 100000 == 0) fprintf(stderr,"NORTH it=%zu pi=%zu sp=%zu gray=%d\n",it,pi,k->sp,k->gray_phase?1:0);
        while(pi<k->ncls){
            size_t ci=k->order[pi];
            if(clause_sat(k,ci) || (!k->gray_phase && is_gray_clause(k,ci))) { pi++; continue; }
            break;
        }
        if(pi>=k->ncls){
            size_t bad=SIZE_MAX; bool any_gray=false;
            for(size_t i=0;i<k->ncls;i++){
                size_t ci=k->order[i];
                if(clause_sat(k,ci)) continue;
                if(!k->gray_phase && is_gray_clause(k,ci)) { any_gray=true; continue; }
                bad=i; break;
            }
            if(bad==SIZE_MAX){
                if(k->sort!=KNOTS_SORT_SATURATION && !k->gray_phase && any_gray){ k->gray_phase=true; pi=0; continue; }
                return true;
            }
            if(k->sort!=KNOTS_SORT_SATURATION && !k->gray_phase){
                bool only_gray=true;
                for(size_t i=0;i<k->ncls;i++){ size_t ci=k->order[i]; if(!clause_sat(k,ci) && !is_gray_clause(k,ci)){ only_gray=false; break; } }
                if(only_gray){ k->gray_phase=true; pi=0; continue; }
            }
            pi=bad;
        }
        pick_next(k,pi);
        size_t ci=k->order[pi];
        if(!k->gray_phase && is_gray_clause(k,ci)){ pi++; continue; }
        if(!any_live(k,ci)) {
            if(k->sort==KNOTS_SORT_SATURATION){
                uint64_t sig=clause_sig(k->cnf,ci); size_t bl=0;
                for(size_t i=0;i<k->ncls;i++) if(clause_sig(k->cnf,i)==sig) bl++;
                fprintf(stderr,"SAT-RANK fail clause=%zu pi=%zu block_len=%zu\n",ci,pi,bl);
            }
            mono_report(k,ci); return false;
        }

        if(clause_conflict(k,ci)){ static size_t cc=0; ++cc; if(cc<20 || cc%50000==0) fprintf(stderr,"NORTH conflict cc=%zu ci=%zu pi=%zu sp=%zu\n",cc,ci,pi,k->sp);
            if(!restate_external(k,ci)) {
                if(!clause_conflict(k,ci)) { pi=k->pos[ci]; continue; }
                if(k->sort!=KNOTS_SORT_SATURATION && !k->gray_phase){ mark_gray(k,ci); pi=0; continue; }
                if(k->sort==KNOTS_SORT_SATURATION){ uint64_t sig=clause_sig(k->cnf,ci); size_t bl=0; for(size_t i=0;i<k->ncls;i++) if(clause_sig(k->cnf,i)==sig) bl++; fprintf(stderr,"SAT-RANK fail clause=%zu pi=%zu block_len=%zu\n",ci,pi,bl); }
                mono_report(k,ci); return false;
            }
            { size_t np=k->pos[k->stk[k->sp-1].clause]; pi = np < pi ? np : 0; }
            continue;
        }

        bool progressed=false;
        for(size_t pass=0; pass<2 && !progressed; pass++){
            for(size_t vi=0; vi<k->vn[ci]; vi++){
                if(k->vs[ci][vi].dead) continue;
                const Lit*cl=cnf_clause(k->cnf,ci); size_t n=cnf_clause_len(k->cnf,ci); uint8_t m=k->vs[ci][vi].mask;
                bool sets_inner=false, compat=true;
                for(size_t i=0;i<n;i++){
                    int want=lit_want(k,cl[i].positive,(m>>i)&1u);
                    int8_t cur=k->val[cl[i].var];
                    if(cur>=0){ if(cur!=want){compat=false;break;} }
                    else sets_inner=true;
                }
                if(!compat) continue;
                if((pass==0)!=sets_inner) continue;
                if(!push_dec(k,ci,vi)) return false;
                Dec*d=&k->stk[k->sp-1]; int ar=apply_variant(k,ci,vi,d);
                if(ar<0) return false;
                if(ar==0){ k->sp--; undo_dec(k,d); continue; }
                progressed=true; pi++; break;
            }
        }
        if(!progressed){ static size_t npc=0; ++npc; if(npc<20 || npc%50000==0) fprintf(stderr,"NORTH noprog npc=%zu ci=%zu pi=%zu sp=%zu\n",npc,ci,pi,k->sp);
            if(restate_external(k,ci)) { size_t np=k->pos[k->stk[k->sp-1].clause]; pi = np < pi ? np : 0; continue; }
            if(k->sort!=KNOTS_SORT_SATURATION && !k->gray_phase){ mark_gray(k,ci); pi=0; continue; }
            bool killed=false;
            for(size_t vi=0; vi<k->vn[ci]; vi++){
                if(k->vs[ci][vi].dead) continue;
                const Lit*cl=cnf_clause(k->cnf,ci); size_t n=cnf_clause_len(k->cnf,ci); uint8_t m=k->vs[ci][vi].mask;
                bool compat=true;
                for(size_t i=0;i<n;i++){ int want=lit_want(k,cl[i].positive,(m>>i)&1u); int8_t cur=k->val[cl[i].var]; if(cur>=0 && cur!=want){compat=false;break;} }
                if(!compat){
                    k->vs[ci][vi].bad++;
                    if(k->keep_bad==0 || k->vs[ci][vi].bad > (uint32_t)k->keep_bad){ k->vs[ci][vi].dead=1; killed=true; }
                }
            }
            if(killed) continue;
            mono_report(k,ci); return false;
        }
    }
}

bool solver_init_north(Solver *s, const SolverConfig *cfg){
    (void)cfg;
    const Cnf *cnf=solver_cnf(s); K k={.cnf=cnf,.nvrs=cnf->n_vars,.ncls=cnf->n_clauses};
    k.val=malloc((k.nvrs?k.nvrs:1)*sizeof*k.val);
    k.owner=malloc((k.nvrs?k.nvrs:1)*sizeof*k.owner);
    k.var_pool=malloc((k.nvrs?k.nvrs:1)*sizeof*k.var_pool);
    k.vs=calloc(k.ncls?k.ncls:1,sizeof*k.vs); k.vn=calloc(k.ncls?k.ncls:1,sizeof*k.vn);
    bool *as=malloc(k.nvrs?k.nvrs:1);
    k.order=malloc((k.ncls?k.ncls:1)*sizeof*k.order);
    k.pos=malloc((k.ncls?k.ncls:1)*sizeof*k.pos);
    k.ref = NULL; k.trace = 0;
    k.random_mode = cfg ? cfg->knots_random_mode : false;
    k.rng = cfg ? (cfg->knots_seed ? cfg->knots_seed : 1u) : 1u;
    k.sort = cfg ? cfg->knots_sort : KNOTS_SORT_NONE;
    k.mono = cfg ? cfg->knots_mono : false;
    k.reverse = cfg ? cfg->knots_reverse : false;
    k.keep_bad = cfg ? cfg->knots_keep_bad : 0;
    k.gray_phase = (k.sort == KNOTS_SORT_SATURATION); /* saturation: no gray deferral */
    k.north = true;
    k.max_dive = cfg ? (cfg->north_max_dive?cfg->north_max_dive:8) : 8;
    {
        const char *t=getenv("KNOTS_TRACE"); if(t && *t) k.trace=atoi(t);
        const char *rp=getenv("KNOTS_REF"); if(rp && *rp){ FILE*f=fopen(rp,"r"); if(f){ bool*rb=malloc(k.nvrs?k.nvrs:1); if(rb){ for(size_t i=0;i<k.nvrs;i++){int c=fgetc(f); if(c=='0'||c=='1') rb[i]=(c=='1'); else rb[i]=false;} k.ref=rb; } fclose(f);} }
    }
    if(!k.val||!k.owner||!k.var_pool||!k.vs||!k.vn||!as||!k.order||!k.pos) goto oom;
    for(size_t v=0;v<k.nvrs;v++){ k.val[v]=-1; k.owner[v]=SIZE_MAX; k.var_pool[v]=SIZE_MAX; }
    for(size_t i=0;i<k.ncls;i++){ build_variants(&k,i); if(!attach_pool(&k,i)) goto oom; }
    fprintf(stderr,"NORTH built pools=%zu\n",k.np);
    build_order(&k, cfg ? cfg->knots_sort : KNOTS_SORT_NONE);
    fprintf(stderr,"NORTH ordered\n");
    bool ok=false;
    size_t rounds = (cfg && cfg->knots_rounds) ? cfg->knots_rounds : 1;
    if(!k.random_mode) rounds = 1;
    for(size_t r=0;r<rounds && !ok;r++){
        for(size_t v=0;v<k.nvrs;v++){ k.val[v]=-1; k.owner[v]=SIZE_MAX; }
        for(size_t i=0;i<k.ncls;i++) for(size_t vi=0;vi<k.vn[i];vi++){ k.vs[i][vi].dead=0; k.vs[i][vi].bad=0; }
        for(size_t i=0;i<k.sp;i++) k.stk[i].cn=0;
        k.sp=0;
        build_order(&k, cfg ? cfg->knots_sort : KNOTS_SORT_NONE);
        k.rng += 0x9e3779b97f4a7c15ULL + r;
        ok = solve(&k);
        if(k.trace) fprintf(stderr,"TRACE round %zu ok=%d\n",r,ok?1:0);
    }
    for(size_t v=0;v<k.nvrs;v++) as[v]= ok ? (k.val[v]>0) : false;
    solver_set_assignment(s,as);
    /* Post-modes are applied by solver_solve on SAT or UNKNOWN for Knots init. */
    for(size_t i=0;i<k.ncls;i++) free(k.vs[i]);
    for(size_t i=0;i<k.sc;i++) free(k.stk[i].chg);
    for(size_t i=0;i<k.np;i++){ free(k.pools[i].vars); free(k.pools[i].cls); }
    free(k.pools); free(k.stk);
    free((void*)k.ref);
    free(k.val); free(k.owner); free(k.var_pool); free(k.vs); free(k.vn); free(k.order); free(k.pos); free(as);
    return ok;
oom:
    if(k.vs) for(size_t i=0;i<k.ncls;i++) free(k.vs[i]);
    for(size_t i=0;i<k.sc;i++) free(k.stk[i].chg);
    for(size_t i=0;i<k.np;i++){ free(k.pools[i].vars); free(k.pools[i].cls); }
    free(k.pools); free(k.stk);
    free((void*)k.ref);
    free(k.val); free(k.owner); free(k.var_pool); free(k.vs); free(k.vn); free(k.order); free(k.pos); free(as);
    return false;
}
