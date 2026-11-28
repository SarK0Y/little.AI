/* M+: CNF as linear inequalities, solved by simple LP relaxation.
 * Clause (l1 v l2 v l3) => sum(f(li)) + s_i >= 1, s_i>=0, 0<=x<=1
 * minimize sum(s_i). Greedy projected coordinate descent on x, then round+repair. */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sat.h"

typedef struct { const Cnf *cnf; bool *a; uint32_t *sat; size_t *rogue; size_t nr; size_t nv; } R;

static void r_rebuild(R *r){
    r->nr=0;
    for(size_t ci=0;ci<r->cnf->n_clauses;ci++){
        const Lit *cl=cnf_clause(r->cnf,ci); size_t n=cnf_clause_len(r->cnf,ci);
        uint32_t c=0; for(size_t k=0;k<n;k++) if(r->a[cl[k].var]==cl[k].positive) c++;
        r->sat[ci]=c; if(c==0) r->rogue[r->nr++]=ci;
    }
}
static void r_flip(R *r,size_t v){
    bool nv=!r->a[v]; r->a[v]=nv;
    r_rebuild(r);
}

static void repair(R *r){
    for(int pass=0;pass<8;pass++){
        size_t flips=0;
        for(size_t v=0;v<r->nv;v++){
            long br=0, mk=0;
            for(size_t ci=0;ci<r->cnf->n_clauses;ci++){
                const Lit *cl=cnf_clause(r->cnf,ci); size_t n=cnf_clause_len(r->cnf,ci);
                bool in=false, pos=false; for(size_t k=0;k<n;k++) if(cl[k].var==v){ in=true; pos=cl[k].positive; break; }
                if(!in) continue;
                if(r->sat[ci]==0) mk++;
                else if(r->sat[ci]==1 && r->a[v]==pos) br++;
            }
            if(mk>br){ r_flip(r,v); flips++; }
        }
        if(!flips) break;
    }
}

bool solver_init_m_plus(Solver *s){
    const Cnf *cnf=solver_cnf(s);
    size_t nv=cnf->n_vars, nc=cnf->n_clauses;
    double *x=calloc(nv?nv:1,sizeof*x);
    double *sl=calloc(nc?nc:1,sizeof*sl);
    bool *a=malloc((nv?nv:1));
    uint32_t *satc=malloc((nc?nc:1)*sizeof*satc);
    size_t *rog=malloc((nc?nc:1)*sizeof*rog);
    if(!x||!sl||!a||!satc||!rog){ free(x); free(sl); free(a); free(satc); free(rog); return false; }
    for(size_t v=0;v<nv;v++) x[v]=0.5;
    for(int it=0;it<200;it++){
        double maxd=0;
        for(size_t v=0;v<nv;v++){
            double g=0;
            for(size_t ci=0;ci<nc;ci++){
                const Lit *cl=cnf_clause(cnf,ci); size_t n=cnf_clause_len(cnf,ci);
                double sum=0; int coeff=0;
                for(size_t k=0;k<n;k++){
                    double xv=x[cl[k].var];
                    if(cl[k].positive){ sum+=xv; if(cl[k].var==v) coeff=1; }
                    else { sum+=1.0-xv; if(cl[k].var==v) coeff=-1; }
                }
                sl[ci]=sum;
                if(sum < 1.0 && coeff!=0) g += -(double)coeff;
            }
            double nx=x[v]-0.2*g;
            if(nx<0) nx=0; if(nx>1) nx=1;
            double d=fabs(nx-x[v]); if(d>maxd) maxd=d;
            x[v]=nx;
        }
        if(maxd<1e-6) break;
    }
    for(size_t v=0;v<nv;v++) a[v]=x[v]>=0.5;
    solver_set_assignment(s,a);
    R r={cnf,a,satc,rog,0,nv};
    r_rebuild(&r);
    repair(&r);
    solver_set_assignment(s,a);
    free(x); free(sl); free(a); free(satc); free(rog);
    return true;
}
