#include "groups.h"

#include <stdlib.h>

GroupConfig group_config_default(void) {
    return (GroupConfig){ .max_groups = 16, .group_len = 0, .directed_only = false, .diversify = 0 };
}

typedef struct {
    size_t *clauses;     /* clause ids (into the full formula) that formed this group */
    size_t n_clauses;
    bool *assignment;    /* full snapshot X_k: satisfies every clause of the group */
} Group;

static int cmp_size(const void *a, const void *b) {
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return (x > y) - (x < y);
}

static uint64_t mix64(uint64_t x) {   /* splitmix64 finalizer: order-independent set hash */
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

/* ======================================================================== */
/* independent solve of one group                                           */
/* ======================================================================== */

/* Sub-formula made only of clauses `ids`; its variables are compacted in
 * ascending original order and orig[i] maps back. */
static bool build_sub(const Cnf *cnf, const size_t *ids, size_t n, Cnf *sub, size_t **orig_out) {
    size_t nv = cnf->n_vars, total = 0, used = 0;
    size_t *map = malloc((nv ? nv : 1) * sizeof *map);
    if (!map) return false;
    for (size_t v = 0; v < nv; v++) map[v] = SIZE_MAX;
    for (size_t i = 0; i < n; i++) {
        const Lit *c = cnf_clause(cnf, ids[i]);
        size_t len = cnf_clause_len(cnf, ids[i]);
        total += len;
        for (size_t k = 0; k < len; k++)
            if (map[c[k].var] == SIZE_MAX) { map[c[k].var] = 0; used++; }
    }
    size_t *orig = malloc((used ? used : 1) * sizeof *orig);
    size_t *off = malloc((n + 1) * sizeof *off);
    Lit *lits = malloc((total ? total : 1) * sizeof *lits);
    if (!orig || !off || !lits) { free(map); free(orig); free(off); free(lits); return false; }
    size_t k = 0;
    for (size_t v = 0; v < nv; v++)
        if (map[v] != SIZE_MAX) { map[v] = k; orig[k++] = v; }
    size_t pos = 0;
    off[0] = 0;
    for (size_t i = 0; i < n; i++) {
        const Lit *c = cnf_clause(cnf, ids[i]);
        size_t len = cnf_clause_len(cnf, ids[i]);
        for (size_t j = 0; j < len; j++) lits[pos++] = (Lit){ (uint32_t)map[c[j].var], c[j].positive };
        off[i + 1] = pos;
    }
    free(map);
    *sub = (Cnf){ used, n, off, lits };
    *orig_out = orig;
    return true;
}

/* 1 = solved (snap filled), 0 = budget ended / unsolvable, -1 = out of memory.
 * The group sees only its own clauses; everything else keeps its value from `base`. */
static int solve_group(const Cnf *cnf, const size_t *ids, size_t n, const bool *base,
                       const SolverConfig *cfg, size_t seed_off, bool *snap, size_t *steps) {
    Cnf sub;
    size_t *orig;
    if (!build_sub(cnf, ids, n, &sub, &orig)) return -1;
    bool *init = malloc(sub.n_vars ? sub.n_vars : 1);
    ResultSolver rs = solver_new(&sub, cfg->seed + seed_off);
    if (!init || !rs.ok) { free(init); free(orig); cnf_free(&sub); return -1; }
    for (size_t i = 0; i < sub.n_vars; i++) init[i] = base[orig[i]];

    SolverConfig sc = *cfg;
    sc.initial = init;
    sc.on_progress = NULL; sc.progress_ctx = NULL; sc.progress_every = 0;
    ResultSolve r = solver_solve(rs.val, &sc);
    solver_free(rs.val);

    int rc = 0;
    if (!r.ok) {
        rc = -1;
    } else {
        *steps += r.val.steps;
        if (r.val.status == SOLVE_SAT && verify(&sub, r.val.assignment, r.val.n_vars)) {
            memcpy(snap, base, cnf->n_vars);
            for (size_t i = 0; i < sub.n_vars; i++) snap[orig[i]] = r.val.assignment[i];
            rc = 1;
        }
        solve_result_free(&r.val);
    }
    free(init); free(orig); cnf_free(&sub);
    return rc;
}

/* ======================================================================== */
/* merge                                                                    */
/* ======================================================================== */

typedef struct {
    Solver *full;            /* whole formula; its assignment IS the merged assignment M */
    const bool *M;
    const Group *groups;
    size_t ng;               /* source index ng is the "free" source: any rogue clause, any polarity */
    const size_t *mem_off;   /* clause -> groups it belongs to, CSR: mem[mem_off[c] .. mem_off[c+1]) */
    const size_t *mem;
    unsigned char *fired;    /* each variable fires at most once for the whole merge */
    size_t *mark;
    size_t stamp;
} Merge;

/* Is clause c one that source g is responsible for? */
static bool in_source(const Merge *m, size_t c, size_t g) {
    if (g >= m->ng) return true;
    for (size_t i = m->mem_off[c]; i < m->mem_off[c + 1]; i++)
        if (m->mem[i] == g) return true;
    return false;
}

/* Neutral right now (zero breaks), unfired, and g's snapshot wants the other polarity. */
static bool fireable_basic(Merge *m, size_t u, size_t g) {
    if (m->fired[u]) return false;
    if (g < m->ng && m->groups[g].assignment[u] == m->M[u]) return false;
    return solver_refresh_var(m->full, u).break_count == 0;
}

/* ...and it still sits in a rogue clause that g is responsible for. */
static bool fireable_full(Merge *m, size_t u, size_t g) {
    if (!fireable_basic(m, u, g)) return false;
    for (size_t i = 0, n = solver_occ_count(m->full, u); i < n; i++) {
        size_t c = solver_occ_clause(m->full, u, i);
        if (solver_clause_is_rogue(m->full, c) && in_source(m, c, g)) return true;
    }
    return false;
}

/* Distinct fireable variables of source g, in rogue-list order. */
static size_t collect(Merge *m, const Cnf *cnf, size_t g, size_t *out) {
    const size_t *rl = solver_rogue_list(m->full);
    size_t nr = solver_unsatisfied_count(m->full), cnt = 0;
    m->stamp++;
    for (size_t i = 0; i < nr; i++) {
        size_t c = rl[i];
        if (!in_source(m, c, g)) continue;
        const Lit *lits = cnf_clause(cnf, c);
        for (size_t k = 0, len = cnf_clause_len(cnf, c); k < len; k++) {
            size_t u = lits[k].var;
            if (m->mark[u] == m->stamp) continue;
            m->mark[u] = m->stamp;
            if (fireable_basic(m, u, g)) out[cnt++] = u;
        }
    }
    return cnt;
}

/* ======================================================================== */
/* driver                                                                   */
/* ======================================================================== */

void group_result_free(GroupResult *r) {
    free(r->assignment); free(r->group_sizes); free(r->group_off); free(r->group_clauses);
    free(r->rogue); free(r->rogue_group_off); free(r->rogue_group);
    free(r->neutral); free(r->prime); free(r->contradictions);
    memset(r, 0, sizeof *r);
}

ResultGroup group_solve(const Cnf *cnf, const SolverConfig *cfg, const GroupConfig *gcfg) {
    GroupResult out;
    memset(&out, 0, sizeof out);
    size_t nv = cnf->n_vars, nc = cnf->n_clauses;
    out.n_vars = nv;
    Error err = {0};
    Group *groups = NULL;
    size_t ng = 0;
    Solver *full = NULL;
    bool *base = NULL;
    size_t *mem_off = NULL, *mem = NULL;
    unsigned char *fired = NULL;
    size_t *mark = NULL, *cand = NULL, *sel = NULL, sel_cap = 0;
    uint64_t hashes[GROUPS_MAX];
    size_t cap = gcfg->max_groups < 1 ? 1 : (gcfg->max_groups > GROUPS_MAX ? GROUPS_MAX : gcfg->max_groups);

#define NOMEM() do { err = error_make(ERR_NOMEM, "out of memory"); goto fail; } while (0)

    if (!cnf_find_contradictions(cnf, &out.contradictions, &out.n_contradictions)) NOMEM();
    if (out.n_contradictions) {
        out.status = SOLVE_UNSAT;
        return OK(ResultGroup, out);
    }

    ResultSolver rs = solver_new(cnf, cfg->seed);
    if (!rs.ok) { err = rs.err; goto fail; }
    full = rs.val;
    groups = calloc(cap, sizeof *groups);
    base = malloc(nv ? nv : 1);
    if (!groups || !base) NOMEM();

    /* ---- formation: each new group = ALL clauses rogue after applying the previous ones ---- */
    if (cfg->init == INIT_RAREST) { if (!solver_init_rarest(full)) NOMEM(); }
    else solver_init_majority(full);
    bool converged = false;
    size_t repeats = 0;
    for (;;) {
        size_t nr = solver_unsatisfied_count(full);
        if (nr == 0) { converged = true; break; }
        if (ng == cap) break;
        const size_t *rl = solver_rogue_list(full);
        size_t take = (gcfg->group_len > 0 && gcfg->group_len < nr) ? gcfg->group_len : nr;
        if (take < nr) {                         /* group_len: the `take` lowest rogue ids go first */
            if (!sel || sel_cap < nr) {
                free(sel);
                sel = malloc(nr * sizeof *sel);
                if (!sel) NOMEM();
                sel_cap = nr;
            }
            memcpy(sel, rl, nr * sizeof *sel);
            qsort(sel, nr, sizeof *sel, cmp_size);
            rl = sel;
        }
        uint64_t h = 0;
        for (size_t i = 0; i < take; i++) h += mix64(rl[i]);
        bool repeat = false;
        for (size_t i = 0; i < ng; i++)
            if (hashes[i] == h && groups[i].n_clauses == take) repeat = true;
        SolverConfig gc = *cfg;
        if (repeat) {                            /* same rogue set as an earlier group: no progress */
            if (repeats >= gcfg->diversify) {
#ifdef GROUPS_TRACE
                fprintf(stderr, "[stop] rogue set of %zu clauses repeats an earlier group\n", nr);
#endif
                break;
            }
            repeats++;
            gc.noise = 1.0;                      /* diversify: random pick among the best class */
        }

        Group *g = &groups[ng];
        g->clauses = malloc(take * sizeof *g->clauses);
        g->assignment = malloc(nv ? nv : 1);
        if (!g->clauses || !g->assignment) NOMEM();
        memcpy(g->clauses, rl, take * sizeof *g->clauses);
        g->n_clauses = take;
        memcpy(base, solver_assignment(full), nv);
        int rc = solve_group(cnf, g->clauses, take, base, &gc, ng + 1000 * repeats, g->assignment, &out.steps);
        if (rc < 0) NOMEM();
        if (rc == 0) break;                      /* group could not be solved: stop forming */
        hashes[ng] = h;
        solver_set_assignment(full, g->assignment);   /* apply, then inspect the entire formula */
#ifdef GROUPS_TRACE
        fprintf(stderr, "[group %zu] %zu of %zu rogue clauses -> %zu rogue after apply; first ids:", ng + 1, take, nr, solver_unsatisfied_count(full));
        for (size_t i = 0; i < take && i < 4; i++) fprintf(stderr, " %zu", g->clauses[i] + 1);
        fputc('\n', stderr);
#endif
        ng++;
    }
    out.n_groups = ng;
    out.group_sizes = malloc((ng ? ng : 1) * sizeof *out.group_sizes);
    if (!out.group_sizes) NOMEM();
    size_t members_total = 0;
    for (size_t k = 0; k < ng; k++) { out.group_sizes[k] = groups[k].n_clauses; members_total += groups[k].n_clauses; }
    out.group_off = malloc((ng + 1) * sizeof *out.group_off);
    out.group_clauses = malloc((members_total ? members_total : 1) * sizeof *out.group_clauses);
    if (!out.group_off || !out.group_clauses) NOMEM();
    out.group_off[0] = 0;
    for (size_t k = 0; k < ng; k++) {
        memcpy(out.group_clauses + out.group_off[k], groups[k].clauses, groups[k].n_clauses * sizeof *groups[k].clauses);
        out.group_off[k + 1] = out.group_off[k] + groups[k].n_clauses;
    }

    /* ---- merge: dynamic anchor, one-shot neutral flips ---- */
    if (!converged && ng > 0) {
        size_t anchor = 0, best_n = 0;
        for (size_t k = 0; k < ng; k++) {
            solver_set_assignment(full, groups[k].assignment);
            const VarStats *st = solver_var_stats(full);
            size_t n = 0;
            for (size_t v = 0; v < nv; v++) n += st[v].state == VAR_NEUTRAL;
            if (k == 0 || n > best_n) { best_n = n; anchor = k; }   /* ties: earliest group */
        }
        solver_set_assignment(full, groups[anchor].assignment);
        out.anchor = anchor;
        out.merged = true;

        size_t members = 0;
        for (size_t k = 0; k < ng; k++) members += groups[k].n_clauses;
        mem_off = calloc(nc + 1, sizeof *mem_off);
        mem = malloc((members ? members : 1) * sizeof *mem);
        size_t *fill = malloc((nc ? nc : 1) * sizeof *fill);
        fired = calloc(nv ? nv : 1, 1);
        mark = calloc(nv ? nv : 1, sizeof *mark);
        cand = malloc((nv ? nv : 1) * sizeof *cand);
        if (!mem_off || !mem || !fill || !fired || !mark || !cand) { free(fill); NOMEM(); }
        for (size_t k = 0; k < ng; k++)
            for (size_t i = 0; i < groups[k].n_clauses; i++) mem_off[groups[k].clauses[i] + 1]++;
        for (size_t c = 0; c < nc; c++) mem_off[c + 1] += mem_off[c];
        memcpy(fill, mem_off, nc * sizeof *fill);
        for (size_t k = 0; k < ng; k++)
            for (size_t i = 0; i < groups[k].n_clauses; i++) mem[fill[groups[k].clauses[i]]++] = k;
        free(fill);

        Merge m = { full, solver_assignment(full), groups, ng, mem_off, mem, fired, mark, 0 };
        while (solver_unsatisfied_count(full) > 0) {
            /* source = group with the most fireable neutrals right now (ties: earliest) */
            size_t best_g = SIZE_MAX, best_cnt = 0;
            for (size_t g = 0; g < ng; g++) {
                size_t cnt = collect(&m, cnf, g, cand);
                if (cnt > best_cnt) { best_cnt = cnt; best_g = g; }
            }
            if (best_cnt == 0 && !gcfg->directed_only) {   /* no directed flip left: free neutrals */
                best_cnt = collect(&m, cnf, ng, cand);
                best_g = ng;
            }
            if (best_cnt == 0) break;            /* neutrals exhausted */
            size_t k = collect(&m, cnf, best_g, cand);
            for (size_t j = 0; j < k; j++) {
                size_t u = cand[j];
                if (!fireable_full(&m, u, best_g)) continue;   /* earlier flips may have changed things */
                solver_flip(full, u);
                fired[u] = 1;
                out.merge_flips++;
                if (best_g == ng) out.merge_flips_free++;
            }
        }
    }

    /* ---- report: merged assignment + diagnostics, verified before claiming SAT ---- */
    out.assignment = malloc(nv ? nv : 1);
    out.n_rogue = solver_unsatisfied_count(full);
    out.rogue = malloc((out.n_rogue ? out.n_rogue : 1) * sizeof *out.rogue);
    if (!out.assignment || !out.rogue) NOMEM();
    memcpy(out.assignment, solver_assignment(full), nv);
    memcpy(out.rogue, solver_rogue_list(full), out.n_rogue * sizeof *out.rogue);
    {   /* which group(s) was each unmerged clause rogue in? (ascending group ids) */
        size_t *ridx = malloc((nc ? nc : 1) * sizeof *ridx);
        out.rogue_group_off = calloc(out.n_rogue + 1, sizeof *out.rogue_group_off);
        size_t *fillp = malloc((out.n_rogue ? out.n_rogue : 1) * sizeof *fillp);
        if (!ridx || !out.rogue_group_off || !fillp) { free(ridx); free(fillp); NOMEM(); }
        for (size_t c = 0; c < nc; c++) ridx[c] = SIZE_MAX;
        for (size_t i = 0; i < out.n_rogue; i++) ridx[out.rogue[i]] = i;
        size_t total = 0;
        for (size_t j = 0; j < members_total; j++) {
            size_t ri = ridx[out.group_clauses[j]];
            if (ri != SIZE_MAX) { out.rogue_group_off[ri + 1]++; total++; }
        }
        for (size_t i = 0; i < out.n_rogue; i++) out.rogue_group_off[i + 1] += out.rogue_group_off[i];
        out.rogue_group = malloc((total ? total : 1) * sizeof *out.rogue_group);
        if (!out.rogue_group) { free(ridx); free(fillp); NOMEM(); }
        memcpy(fillp, out.rogue_group_off, out.n_rogue * sizeof *fillp);
        for (size_t k = 0; k < ng; k++)
            for (size_t j = out.group_off[k]; j < out.group_off[k + 1]; j++) {
                size_t ri = ridx[out.group_clauses[j]];
                if (ri != SIZE_MAX) out.rogue_group[fillp[ri]++] = k;
            }
        free(ridx); free(fillp);
    }
    {
        const VarStats *st = solver_var_stats(full);
        size_t nn = 0, np = 0;
        for (size_t v = 0; v < nv; v++) { nn += st[v].state == VAR_NEUTRAL; np += st[v].state == VAR_PRIME; }
        out.neutral = malloc((nn ? nn : 1) * sizeof *out.neutral);
        out.prime = malloc((np ? np : 1) * sizeof *out.prime);
        if (!out.neutral || !out.prime) NOMEM();
        for (size_t v = 0; v < nv; v++) {
            if (st[v].state == VAR_NEUTRAL) out.neutral[out.n_neutral++] = v;
            else if (st[v].state == VAR_PRIME) out.prime[out.n_prime++] = v;
        }
    }
    if (out.n_rogue == 0) {
        if (!verify(cnf, out.assignment, nv)) { err = error_make(ERR_VERIFY, "merged assignment failed verification"); goto fail; }
        out.status = SOLVE_SAT;
    } else {
        out.status = SOLVE_UNKNOWN;
    }

    for (size_t k = 0; k < cap; k++) { free(groups[k].clauses); free(groups[k].assignment); }
    free(groups); free(base); free(sel); free(mem_off); free(mem); free(fired); free(mark); free(cand);
    solver_free(full);
    return OK(ResultGroup, out);

fail:
    if (groups) for (size_t k = 0; k < cap; k++) { free(groups[k].clauses); free(groups[k].assignment); }
    free(groups); free(base); free(sel); free(mem_off); free(mem); free(fired); free(mark); free(cand);
    solver_free(full);
    group_result_free(&out);
    return ERR(ResultGroup, err);
#undef NOMEM
}
