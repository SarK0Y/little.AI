/* Knots: pool-based clause solver. Spec: knowledge/c_3sat/notes/knots.txt. */
#include "sat.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { uint8_t mask; uint8_t dead; } Varnt;
typedef struct { size_t clause; size_t vi; size_t *chg; size_t cn, cap; } Dec;
typedef struct { size_t *vars; size_t nv, cv; size_t *cls; size_t nc, cc; } Pool;
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
    for(size_t m=1;m<mmax;m++){ vs[vn].mask=(uint8_t)m; vs[vn].dead=0; vn++; }
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
    if(k->sp==k->sc){ size_t c=k->sc?k->sc*2:32; Dec*ns=realloc(k->stk,c*sizeof*ns); if(!ns) return false; for(size_t i=k->sc;i<c;i++){ns[i].chg=NULL;ns[i].cn=0;ns[i].cap=0;} k->stk=ns;k->sc=c; }
    Dec*d=&k->stk[k->sp++]; d->clause=clause; d->vi=vi; d->cn=0; return true;
}
static bool dec_add(Dec*d,size_t var){
    if(d->cn==d->cap){ size_t c=d->cap?d->cap*2:4; size_t*n=realloc(d->chg,c*sizeof*n); if(!n) return false; d->chg=n; d->cap=c; }
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
static void clear_dead_after(K*k,size_t jcl){
    /* clear dead marks for clauses after jcl in same pool only; other pools independent */
    size_t pid = clause_pool(k,jcl);
    size_t jpos = k->pos[jcl];
    for(size_t p=jpos+1;p<k->ncls;p++){
        size_t i=k->order[p];
        if(pid!=SIZE_MAX && clause_pool(k,i)!=pid) continue;
        for(size_t vi=0;vi<k->vn[i];vi++) k->vs[i][vi].dead=0;
    }
}

static void undo_dec(K*k,Dec*d){
    for(size_t i=0;i<d->cn;i++){ size_t v=d->chg[i]; if(k->owner[v]==d->clause) k->owner[v]=SIZE_MAX; k->val[v]=-1; }
    d->cn=0;
}

static int apply_variant(K*k,size_t ci,size_t vi,Dec*d){
    const Lit*cl=cnf_clause(k->cnf,ci); size_t n=cnf_clause_len(k->cnf,ci); uint8_t m=k->vs[ci][vi].mask;
    for(size_t i=0;i<n;i++){ int want=(m&(1u<<i))?(cl[i].positive?1:0):(cl[i].positive?0:1); int8_t cur=k->val[cl[i].var]; if(cur>=0 && cur!=want) return 0; }
    for(size_t i=0;i<n;i++){
        int want=(m&(1u<<i))?(cl[i].positive?1:0):(cl[i].positive?0:1);
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
            bool bad=false; for(size_t i=0;i<n;i++){ int want=(m&(1u<<i))?(cl[i].positive?1:0):(cl[i].positive?0:1); bool rv=k->ref[cl[i].var]; if((want?1:0)!=(rv?1:0)) bad=true; }
            if(bad) fprintf(stderr,"TRACE restate jcl=%zu vi=%zu deviates-from-ref\n",jcl,vi);
        }
        return true;
    }
    /* exhausted all variants after old_vi in this context; mark old_vi dead? no, try from 0 with dead marks? */
    k->vs[jcl][old_vi].dead=1;
    for(size_t vi=0;vi<k->vn[jcl];vi++){
        if(k->vs[jcl][vi].dead) continue;
        if(!push_dec(k,jcl,vi)) return false;
        Dec*d=&k->stk[k->sp-1]; int ar=apply_variant(k,jcl,vi,d);
        if(ar<0) return false;
        if(ar==0){ k->sp--; undo_dec(k,d); k->vs[jcl][vi].dead=1; continue; }
        if(k->trace && k->ref){
            const Lit*cl=cnf_clause(k->cnf,jcl); size_t n=cnf_clause_len(k->cnf,jcl); uint8_t m=k->vs[jcl][vi].mask;
            bool bad=false; for(size_t i=0;i<n;i++){ int want=(m&(1u<<i))?(cl[i].positive?1:0):(cl[i].positive?0:1); bool rv=k->ref[cl[i].var]; if((want?1:0)!=(rv?1:0)) bad=true; }
            if(bad) fprintf(stderr,"TRACE restate-wrap jcl=%zu vi=%zu deviates-from-ref\n",jcl,vi);
        }
        return true;
    }
    return false;
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

static bool restate_external(K*k,size_t ci){
    /* 5: external vars needed -> jump to orig pool where they are internal, restate.
     * 4: exploit inner vars of given pool first / keep jumps local when pool is inner-only. */
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
        if(is_local){ if(ln<8) local[ln++]=od; }
        else { if(en<8) ext[en++]=od; }
    }
    for(size_t i=0;i<ln;i++) for(size_t j=i+1;j<ln;j++) if(local[j]>local[i]){size_t t=local[i];local[i]=local[j];local[j]=t;}
    for(size_t i=0;i<en;i++) for(size_t j=i+1;j<en;j++) if(ext[j]>ext[i]){size_t t=ext[i];ext[i]=ext[j];ext[j]=t;}
    /* try local pool first */
    for(size_t i=0;i<ln;i++) if(try_restate_at(k,local[i])) return true;
    /* if pool has no external vars, do not jump outside */
    if(pid!=SIZE_MAX && !pool_has_external_set(k,pid)) return false;
    for(size_t i=0;i<en;i++) if(try_restate_at(k,ext[i])) return true;
    /* fallback: any decision, but prefer local pool */
    for(size_t i=k->sp;i>0;i--){
        size_t od=i-1; size_t oc=k->stk[od].clause;
        if(pid!=SIZE_MAX && clause_pool(k,oc)!=pid) continue;
        if(try_restate_at(k,od)) return true;
    }
    if(pid!=SIZE_MAX && !pool_has_external_set(k,pid)) return false;
    while(k->sp>0){ if(try_restate_at(k,k->sp-1)) return true; }
    return false;
}

static bool solve(K*k){
    size_t pi=0;
    while(1){
        while(pi<k->ncls && clause_sat(k,k->order[pi])) pi++;
        if(pi>=k->ncls) return true;
        size_t ci=k->order[pi];
        if(!any_live(k,ci)) { if(k->trace) fprintf(stderr,"TRACE fatal empty ci=%zu pool=%zu sp=%zu\n",ci,clause_pool(k,ci),k->sp); return false; }

        if(clause_conflict(k,ci)){
            bool conf_before=true;
            if(!restate_external(k,ci)) {
                if(k->trace){
                    size_t pid=clause_pool(k,ci);
                    fprintf(stderr,"TRACE fatal conflict ci=%zu pool=%zu sp=%zu ext=%d conf_now=%d\n",ci,pid,k->sp,(pid!=SIZE_MAX && pool_has_external_set(k,pid))?1:0, clause_conflict(k,ci)?1:0);
                    const Lit*cl=cnf_clause(k->cnf,ci); size_t n=cnf_clause_len(k->cnf,ci);
                    for(size_t i=0;i<n;i++) fprintf(stderr,"  lit v%u=%d owner=%zu od=%zu\n",cl[i].var, (int)k->val[cl[i].var], k->owner[cl[i].var], owner_dec(k,cl[i].var));
                    fprintf(stderr,"  live ci=%d\n", any_live(k,ci)?1:0);
                    for(size_t vi=0;vi<k->vn[ci];vi++) fprintf(stderr,"  v[%zu] dead=%u\n",vi,k->vs[ci][vi].dead);
                    for(size_t i=k->sp;i>0 && i+12>k->sp;i--){ Dec*d=&k->stk[i-1]; fprintf(stderr,"  dec clause=%zu vi=%zu/%zu dead=%u\n",d->clause,d->vi,k->vn[d->clause],k->vs[d->clause][d->vi].dead); }
                }
                (void)conf_before;
                return false;
            }
            if(k->trace>=2) fprintf(stderr,"TRACE backtrack to clause=%zu sp=%zu\n",k->stk[k->sp-1].clause,k->sp);
            pi=k->pos[k->stk[k->sp-1].clause];
            continue;
        }

        /* 4: inner vars first */
        bool progressed=false;
        for(size_t pass=0; pass<2 && !progressed; pass++){
            for(size_t vi=0; vi<k->vn[ci]; vi++){
                if(k->vs[ci][vi].dead) continue;
                const Lit*cl=cnf_clause(k->cnf,ci); size_t n=cnf_clause_len(k->cnf,ci); uint8_t m=k->vs[ci][vi].mask;
                bool sets_inner=false, compat=true;
                for(size_t i=0;i<n;i++){
                    int want=(m&(1u<<i))?(cl[i].positive?1:0):(cl[i].positive?0:1);
                    int8_t cur=k->val[cl[i].var];
                    if(cur>=0){ if(cur!=want){compat=false;break;} }
                    else sets_inner=true;
                }
                if(!compat) continue;
                if((pass==0)!=sets_inner) continue;
                if(!push_dec(k,ci,vi)) return false;
                Dec*d=&k->stk[k->sp-1]; int ar=apply_variant(k,ci,vi,d);
                if(ar<0) return false;
                if(ar==0){ k->sp--; undo_dec(k,d); k->vs[ci][vi].dead=1; continue; }
                if(k->trace && k->ref){
                    const Lit*cl2=cnf_clause(k->cnf,ci); size_t n2=cnf_clause_len(k->cnf,ci); uint8_t m2=k->vs[ci][vi].mask;
                    bool bad=false; for(size_t i=0;i<n2;i++){ int want=(m2&(1u<<i))?(cl2[i].positive?1:0):(cl2[i].positive?0:1); bool rv=k->ref[cl2[i].var]; if((want?1:0)!=(rv?1:0)) bad=true; }
                    if(bad) fprintf(stderr,"TRACE pick ci=%zu vi=%zu deviates-from-ref\n",ci,vi);
                }
                progressed=true; pi++; break;
            }
        }
        if(!progressed){
            if(!restate_external(k,ci)) {
                size_t pid=clause_pool(k,ci);
                if(pid==SIZE_MAX || !pool_has_external_set(k,pid)) return false;
                return false;
            }
            pi=k->pos[k->stk[k->sp-1].clause];
        }
    }
}

bool solver_init_knots(Solver *s, const SolverConfig *cfg){
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
    {
        const char *t=getenv("KNOTS_TRACE"); if(t && *t) k.trace=atoi(t);
        const char *rp=getenv("KNOTS_REF"); if(rp && *rp){ FILE*f=fopen(rp,"r"); if(f){ bool*rb=malloc(k.nvrs?k.nvrs:1); if(rb){ for(size_t i=0;i<k.nvrs;i++){int c=fgetc(f); if(c=='0'||c=='1') rb[i]=(c=='1'); else rb[i]=false;} k.ref=rb; } fclose(f);} }
    }
    if(!k.val||!k.owner||!k.var_pool||!k.vs||!k.vn||!as||!k.order||!k.pos) goto oom;
    for(size_t v=0;v<k.nvrs;v++){ k.val[v]=-1; k.owner[v]=SIZE_MAX; k.var_pool[v]=SIZE_MAX; }
    for(size_t i=0;i<k.ncls;i++){ build_variants(&k,i); if(!attach_pool(&k,i)) goto oom; }
    /* process clauses grouped by pool to exploit inner-only pools locally */
    {
        size_t oi=0; bool *seen=calloc(k.ncls?k.ncls:1,1);
        if(!seen) goto oom;
        for(size_t i=0;i<k.ncls;i++){
            if(seen[i]) continue;
            size_t pid = clause_pool(&k,i);
            if(pid==SIZE_MAX){ k.order[oi++]=i; seen[i]=true; continue; }
            Pool *p=&k.pools[pid];
            for(size_t j=0;j<p->nc;j++){ size_t ci=p->cls[j]; if(!seen[ci]){ k.order[oi++]=ci; seen[ci]=true; } }
            if(!seen[i]){ k.order[oi++]=i; seen[i]=true; }
        }
        for(size_t i=0;i<k.ncls;i++) if(!seen[i]) k.order[oi++]=i;
        for(size_t i=0;i<k.ncls;i++) k.pos[k.order[i]]=i;
        free(seen);
    }
    bool ok=solve(&k);
    for(size_t v=0;v<k.nvrs;v++) as[v]= ok ? (k.val[v]>0) : false;
    solver_set_assignment(s,as);
    for(size_t i=0;i<k.ncls;i++) free(k.vs[i]);
    for(size_t i=0;i<k.sp;i++) free(k.stk[i].chg);
    for(size_t i=0;i<k.np;i++){ free(k.pools[i].vars); free(k.pools[i].cls); }
    free(k.pools); free(k.stk);
    free((void*)k.ref);
    free(k.val); free(k.owner); free(k.var_pool); free(k.vs); free(k.vn); free(k.order); free(k.pos); free(as);
    return ok;
oom:
    if(k.vs) for(size_t i=0;i<k.ncls;i++) free(k.vs[i]);
    for(size_t i=0;i<k.sp;i++) free(k.stk[i].chg);
    for(size_t i=0;i<k.np;i++){ free(k.pools[i].vars); free(k.pools[i].cls); }
    free(k.pools); free(k.stk);
    free((void*)k.ref);
    free(k.val); free(k.owner); free(k.var_pool); free(k.vs); free(k.vn); free(k.order); free(k.pos); free(as);
    return false;
}
