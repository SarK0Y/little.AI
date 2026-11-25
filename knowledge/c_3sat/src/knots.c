/* Knots: pool-based clause solver. Spec: notes/knots.txt. */
#include "sat.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t mask; /* bit i: this literal chosen true */
    uint8_t dead;
} Varnt;

typedef struct {
    const Cnf *cnf;
    size_t nv, nc;
    int8_t *val;      /* -1/0/1 */
    size_t *level;    /* decision level per var, SIZE_MAX unset */
    size_t *owner;    /* var -> clause index where set as inner, SIZE_MAX */
    Varnt **vs; size_t *vn;
} K;

static bool lit_is_true(const Lit *l, int8_t v) { return v >= 0 && ((v == 1) == l->positive); }
static bool lit_is_false(const Lit *l, int8_t v) { return v >= 0 && ((v == 1) != l->positive); }

static void build_variants(K *k, size_t ci) {
    const Lit *cl = cnf_clause(k->cnf, ci);
    size_t n = cnf_clause_len(k->cnf, ci);
    size_t mmax = 1u << n;
    Varnt *vs = malloc((mmax ? mmax : 1) * sizeof *vs);
    size_t vn = 0;
    if (!vs) { k->vs[ci] = NULL; k->vn[ci] = 0; return; }
    for (size_t m = 1; m < mmax; m++) { /* at least one true literal */
        vs[vn].mask = (uint8_t)m;
        vs[vn].dead = 0;
        vn++;
    }
    k->vs[ci] = vs;
    k->vn[ci] = vn;
}

static bool clause_sat(K *k, size_t ci) {
    const Lit *cl = cnf_clause(k->cnf, ci); size_t n = cnf_clause_len(k->cnf, ci);
    for (size_t i = 0; i < n; i++) if (lit_is_true(&cl[i], k->val[cl[i].var])) return true;
    return false;
}
static bool clause_conflict(K *k, size_t ci) {
    const Lit *cl = cnf_clause(k->cnf, ci); size_t n = cnf_clause_len(k->cnf, ci);
    for (size_t i = 0; i < n; i++) if (!lit_is_false(&cl[i], k->val[cl[i].var])) return false;
    return true;
}

static bool apply_variant(K *k, size_t ci, size_t vi, size_t *chg, size_t *cn) {
    const Lit *cl = cnf_clause(k->cnf, ci); size_t n = cnf_clause_len(k->cnf, ci);
    uint8_t m = k->vs[ci][vi].mask;
    *cn = 0;
    for (size_t i = 0; i < n; i++) {
        int want = (m & (1u << i)) ? (cl[i].positive ? 1 : 0) : (cl[i].positive ? 0 : 1);
        int8_t cur = k->val[cl[i].var];
        if (cur >= 0) { if (cur != want) { for (size_t j=0;j<*cn;j++) k->val[chg[j]]=-1; *cn=0; return false; } }
        else { k->val[cl[i].var] = (int8_t)want; chg[(*cn)++] = cl[i].var; if (k->owner[cl[i].var]==SIZE_MAX) k->owner[cl[i].var]=ci; }
    }
    return true;
}

static bool solve(K *k, size_t ci) {
    while (ci < k->nc && clause_sat(k, ci)) ci++;
    if (ci >= k->nc) return true;
    if (clause_conflict(k, ci)) return false;

    const Lit *cl = cnf_clause(k->cnf, ci); size_t n = cnf_clause_len(k->cnf, ci);
    /* spec 4: inner vars first - vars not seen before or owned by this pool/earlier */
    for (size_t pass = 0; pass < 2; pass++) {
        for (size_t vi = 0; vi < k->vn[ci]; vi++) {
            if (k->vs[ci][vi].dead) continue;
            uint8_t m = k->vs[ci][vi].mask;
            bool touches_external_set = false, sets_inner = false;
            bool ok = true;
            for (size_t i = 0; i < n; i++) {
                int want = (m & (1u << i)) ? (cl[i].positive ? 1 : 0) : (cl[i].positive ? 0 : 1);
                int8_t cur = k->val[cl[i].var];
                if (cur >= 0) { if (cur != want) { ok = false; break; } }
                else sets_inner = true;
                (void)touches_external_set;
            }
            if (!ok) continue;
            if ((pass == 0) != sets_inner) continue;
            size_t chg[8], cn = 0;
            if (!apply_variant(k, ci, vi, chg, &cn)) continue;
            if (solve(k, ci + 1)) return true;
            for (size_t j = 0; j < cn; j++) { if (k->owner[chg[j]]==ci) k->owner[chg[j]]=SIZE_MAX; k->val[chg[j]] = -1; }
        }
    }
    /* spec 5.1: all variants failed for this path -> dead? persistent dead leads to incompleteness in DFS;
       here failure is path-local; caller backtracks. */
    return false;
}

bool solver_init_knots(Solver *s, const SolverConfig *cfg) {
    (void)cfg;
    const Cnf *cnf = solver_cnf(s);
    K k = {.cnf = cnf, .nv = cnf->n_vars, .nc = cnf->n_clauses};
    k.val = malloc((k.nv ? k.nv : 1) * sizeof *k.val);
    k.level = malloc((k.nv ? k.nv : 1) * sizeof *k.level);
    k.owner = malloc((k.nv ? k.nv : 1) * sizeof *k.owner);
    k.vs = calloc(k.nc ? k.nc : 1, sizeof *k.vs);
    k.vn = calloc(k.nc ? k.nc : 1, sizeof *k.vn);
    bool *as = malloc(k.nv ? k.nv : 1);
    if (!k.val || !k.level || !k.owner || !k.vs || !k.vn || !as) goto oom;
    for (size_t v = 0; v < k.nv; v++) { k.val[v] = -1; k.level[v] = SIZE_MAX; k.owner[v] = SIZE_MAX; }
    for (size_t i = 0; i < k.nc; i++) build_variants(&k, i);

    bool ok = solve(&k, 0);
    for (size_t v = 0; v < k.nv; v++) as[v] = ok ? (k.val[v] > 0) : false;
    solver_set_assignment(s, as);

    for (size_t i = 0; i < k.nc; i++) free(k.vs[i]);
    free(k.val); free(k.level); free(k.owner); free(k.vs); free(k.vn); free(as);
    return ok;
oom:
    if (k.vs) for (size_t i = 0; i < k.nc; i++) free(k.vs[i]);
    free(k.val); free(k.level); free(k.owner); free(k.vs); free(k.vn); free(as);
    return false;
}
