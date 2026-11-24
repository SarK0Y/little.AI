#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "groups.h"
#include "sat.h"

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static Cnf must_parse(const char *s) {
    ResultCnf r = cnf_from_dimacs_str(s);
    if (!r.ok) { fprintf(stderr, "unexpected parse error: %s\n", r.err.msg); exit(1); }
    return r.val;
}
static Solver *must_solver(const Cnf *c, uint64_t seed) {
    ResultSolver r = solver_new(c, seed);
    if (!r.ok) { fprintf(stderr, "solver_new: %s\n", r.err.msg); exit(1); }
    return r.val;
}

static void test_dimacs_multiline_and_comments(void) {
    Cnf c = must_parse("c demo\np cnf 3 2\n1 -2\n0\n3 0\n");
    CHECK(c.n_vars == 3 && c.n_clauses == 2);
    CHECK(cnf_clause_len(&c, 0) == 2 && cnf_clause_len(&c, 1) == 1);
    cnf_free(&c);
}

static void test_dimacs_percent_eof(void) {
    Cnf c = must_parse("p cnf 2 1\n1 2 0\n%\n0\n");
    CHECK(c.n_clauses == 1 && cnf_clause_len(&c, 0) == 2);
    cnf_free(&c);
}

static void test_normalization(void) {
    Cnf c = must_parse("p cnf 3 3\n1 1 2 0\n1 -1 2 0\n-3 0\n");
    CHECK(c.n_clauses == 2);                 /* tautology dropped */
    CHECK(cnf_clause_len(&c, 0) == 2);       /* duplicate literal removed */
    cnf_free(&c);
}

static void test_no_twins(void) {
    /* Literal order is irrelevant. Keep the first of a sync pair and the first
     * of an async pair (all signs inverted); retain a merely partial inversion. */
    Cnf c = must_parse("p cnf 3 5\n1 2 3 0\n3 1 2 0\n-1 -2 -3 0\n1 -2 3 0\n-1 2 -3 0\n");
    Cnf filtered = {0};
    CHECK(no_twins(&c, &filtered));
    CHECK(filtered.n_clauses == 2);
    CHECK(cnf_clause_len(&filtered, 0) == 3 && cnf_clause_len(&filtered, 1) == 3);
    cnf_free(&filtered);
    cnf_free(&c);
}

static void test_parse_errors(void) {
    ResultCnf r;
    r = cnf_from_dimacs_str("1 2 0\n");                 CHECK(!r.ok && r.err.kind == ERR_PARSE);
    r = cnf_from_dimacs_str("p cnf 2 1\n1 2\n");        CHECK(!r.ok && strstr(r.err.msg, "unterminated"));
    r = cnf_from_dimacs_str("p cnf 2 1\n3 0\n");        CHECK(!r.ok && strstr(r.err.msg, "outside"));
    r = cnf_from_dimacs_str("p cnf 2 1\n1 x 0\n");      CHECK(!r.ok && strstr(r.err.msg, "invalid DIMACS token"));
    r = cnf_from_dimacs_str("p cnf 2 1\n1 0\n2 0\n");   CHECK(!r.ok && strstr(r.err.msg, "declares"));
    r = cnf_from_dimacs_str("p dnf 2 1\n");             CHECK(!r.ok);
    r = cnf_from_dimacs_file("/definitely/not/here");   CHECK(!r.ok && r.err.kind == ERR_IO);
}

static void test_verify(void) {
    Cnf c = must_parse("p cnf 2 2\n1 2 0\n-1 2 0\n");
    bool good[] = { false, true }, bad[] = { true, false };
    CHECK(verify(&c, good, 2));
    CHECK(!verify(&c, bad, 2));
    CHECK(!verify(&c, good, 1));
    cnf_free(&c);
}

static void test_solves_simple(void) {
    ResultCnf rc = cnf_from_dimacs_file("tests/fixtures/small_sat.cnf");
    CHECK(rc.ok);
    if (!rc.ok) return;
    Solver *s = must_solver(&rc.val, 42);
    SolverConfig cfg = solver_config_default();
    cfg.max_steps = 1000; cfg.restarts = 5; cfg.noise = 0.2; cfg.seed = 42;
    ResultSolve r = solver_solve(s, &cfg);
    CHECK(r.ok && r.val.status == SOLVE_SAT && verify(&rc.val, r.val.assignment, r.val.n_vars));
    if (r.ok) solve_result_free(&r.val);
    solver_free(s); cnf_free(&rc.val);
}

static void test_empty_clause_and_empty_formula(void) {
    Cnf c = must_parse("p cnf 1 1\n0\n");
    Solver *s = must_solver(&c, 1);
    SolverConfig cfg = solver_config_default();
    ResultSolve r = solver_solve(s, &cfg);
    CHECK(r.ok && r.val.status == SOLVE_UNKNOWN);
    solver_free(s); cnf_free(&c);

    Cnf e = must_parse("p cnf 2 0\n");
    s = must_solver(&e, 1);
    r = solver_solve(s, &cfg);
    CHECK(r.ok && r.val.status == SOLVE_SAT);
    if (r.ok) solve_result_free(&r.val);
    solver_free(s); cnf_free(&e);
}

static void test_unsat_is_unknown_never_unsat(void) {
    /* all 8 sign patterns over 3 vars: unsatisfiable */
    Cnf c = must_parse("p cnf 3 8\n1 2 3 0\n1 2 -3 0\n1 -2 3 0\n1 -2 -3 0\n"
                       "-1 2 3 0\n-1 2 -3 0\n-1 -2 3 0\n-1 -2 -3 0\n");
    Solver *s = must_solver(&c, 3);
    SolverConfig cfg = solver_config_default();
    cfg.max_steps = 200; cfg.restarts = 3;
    ResultSolve r = solver_solve(s, &cfg);
    CHECK(r.ok && r.val.status == SOLVE_UNKNOWN && r.val.best_unsatisfied >= 1);
    solver_free(s); cnf_free(&c);
}

static void test_rogue_is_a_clause_state(void) {
    Cnf c = must_parse("p cnf 2 2\n1 0\n-1 2 0\n");
    Solver *s = must_solver(&c, 7);  /* initial assignment is all false */
    CHECK(solver_clause_is_rogue(s, 0));
    CHECK(!solver_clause_is_rogue(s, 1));
    const VarStats *v = solver_var_stats(s);
    CHECK(v[0].state == VAR_PRIME);          /* sole support in clause 1... */
    CHECK(v[0].rogue_occurrences == 1);      /* ...while also in a rogue clause */
    solver_free(s); cnf_free(&c);
}

static void test_neutral_can_occur_in_rogue_clause(void) {
    Cnf c = must_parse("p cnf 2 2\n1 0\n-1 2 0\n");
    Solver *s = must_solver(&c, 5);
    bool a[] = { false, true };
    solver_set_assignment(s, a);
    CHECK(solver_clause_is_rogue(s, 0));
    const VarStats *v = solver_var_stats(s);
    CHECK(v[0].state == VAR_NEUTRAL && v[0].min_support == 2);
    CHECK(v[0].rogue_occurrences == 1);
    solver_free(s); cnf_free(&c);
}

static void test_prime_precedence(void) {
    Cnf c = must_parse("p cnf 3 2\n1 2 0\n1 -2 3 0\n");
    Solver *s = must_solver(&c, 11);
    bool a[] = { true, false, true };
    solver_set_assignment(s, a);
    CHECK(solver_var_stats(s)[0].state == VAR_PRIME);
    solver_free(s); cnf_free(&c);
}

static void test_flip_updates_state(void) {
    Cnf c = must_parse("p cnf 2 2\n1 0\n-1 2 0\n");
    Solver *s = must_solver(&c, 3);
    CHECK(solver_clause_is_rogue(s, 0) && !solver_clause_is_rogue(s, 1));
    solver_flip(s, 0);
    CHECK(!solver_clause_is_rogue(s, 0) && solver_clause_is_rogue(s, 1));
    CHECK(solver_unsatisfied_count(s) == 1);
    CHECK(solver_var_stats(s)[0].rogue_occurrences == 1);
    solver_free(s); cnf_free(&c);
}

/* Lazy bookkeeping must match a from-scratch rebuild after many flips. */
static void test_incremental_matches_rebuild(void) {
    Cnf c = must_parse("p cnf 6 7\n1 2 3 0\n-1 4 5 0\n-2 -4 6 0\n3 -5 -6 0\n"
                       "1 -3 6 0\n-1 -2 -6 0\n2 4 -5 0\n");
    Solver *a = must_solver(&c, 9), *b = must_solver(&c, 9);
    Rng rng = rng_new(123);
    for (int i = 0; i < 500; i++) solver_flip(a, rng_index(&rng, c.n_vars));
    solver_set_assignment(b, solver_assignment(a));
    CHECK(solver_unsatisfied_count(a) == solver_unsatisfied_count(b));
    for (size_t ci = 0; ci < c.n_clauses; ci++)
        CHECK(solver_clause_support(a, ci) == solver_clause_support(b, ci));
    const VarStats *sa = solver_var_stats(a), *sb = solver_var_stats(b);
    for (size_t v = 0; v < c.n_vars; v++) {
        CHECK(sa[v].state == sb[v].state && sa[v].min_support == sb[v].min_support);
        CHECK(sa[v].break_count == sb[v].break_count && sa[v].make_count == sb[v].make_count);
        CHECK(sa[v].satisfied_occurrences == sb[v].satisfied_occurrences);
    }
    solver_free(a); solver_free(b); cnf_free(&c);
}

static void test_contradictory_units_are_unsat(void) {
    Cnf c = must_parse("p cnf 3 5\n1 2 0\n1 0\n3 0\n-1 0\n-3 0\n");
    Solver *s = must_solver(&c, 1);
    SolverConfig cfg = solver_config_default();
    ResultSolve r = solver_solve(s, &cfg);
    CHECK(r.ok && r.val.status == SOLVE_UNSAT);
    CHECK(r.ok && r.val.assignment == NULL && r.val.n_contradictions == 2);
    if (r.ok && r.val.n_contradictions == 2) {
        CHECK(r.val.contradictions[0].a == 1 && r.val.contradictions[0].b == 3);
        CHECK(r.val.contradictions[1].a == 2 && r.val.contradictions[1].b == 4);
    }
    if (r.ok) solve_result_free(&r.val);
    solver_free(s); cnf_free(&c);

    /* negative unit first: the pair is still reported as (positive, negative) */
    Cnf d = must_parse("p cnf 1 2\n-1 0\n1 0\n");
    s = must_solver(&d, 1);
    r = solver_solve(s, &cfg);
    CHECK(r.ok && r.val.status == SOLVE_UNSAT && r.val.n_contradictions == 1);
    if (r.ok && r.val.n_contradictions == 1)
        CHECK(r.val.contradictions[0].a == 1 && r.val.contradictions[0].b == 0);
    if (r.ok) solve_result_free(&r.val);
    solver_free(s); cnf_free(&d);
}

/* core relatives (1 2)/(-1 -2) are consistent; only the extra units (1)/(-1) conflict. */
static void test_contradiction_lives_in_extras(void) {
    Cnf c = must_parse("p cnf 2 4\n1 2 0\n-1 -2 0\n1 0\n-1 0\n");
    Solver *s = must_solver(&c, 1);
    SolverConfig cfg = solver_config_default();
    ResultSolve r = solver_solve(s, &cfg);
    CHECK(r.ok && r.val.status == SOLVE_UNSAT && r.val.n_contradictions == 1);
    if (r.ok && r.val.n_contradictions == 1)
        CHECK(r.val.contradictions[0].a == 2 && r.val.contradictions[0].b == 3);
    if (r.ok) solve_result_free(&r.val);
    solver_free(s); cnf_free(&c);
}

/* (1 2 3) and (-1 -2 -3) are not contradictory: x1=1, x2=0, x3=0 satisfies both. */
static void test_negated_3clauses_are_not_unsat(void) {
    Cnf c = must_parse("p cnf 3 2\n1 2 3 0\n-1 -2 -3 0\n");
    Solver *s = must_solver(&c, 1);
    SolverConfig cfg = solver_config_default();
    cfg.max_steps = 1000; cfg.restarts = 5;
    ResultSolve r = solver_solve(s, &cfg);
    CHECK(r.ok && r.val.status == SOLVE_SAT);
    if (r.ok && r.val.status == SOLVE_SAT) CHECK(verify(&c, r.val.assignment, r.val.n_vars));
    if (r.ok) solve_result_free(&r.val);
    solver_free(s); cnf_free(&c);
}


/* ---- helpers for random formulas ---------------------------------------- */

static Cnf random_cnf(int n, int m, uint64_t seed) {
    size_t cap = (size_t)m * 48 + 64;
    char *text = malloc(cap);
    Rng g = rng_new(seed);
    size_t len = (size_t)snprintf(text, cap, "p cnf %d %d\n", n, m);
    for (int i = 0; i < m; i++) {
        int v[3];
        for (int k = 0; k < 3; k++) {
            int x, dup;
            do { x = 1 + (int)rng_index(&g, (size_t)n); dup = 0; for (int j = 0; j < k; j++) dup |= (v[j] == x); } while (dup);
            v[k] = x;
        }
        for (int k = 0; k < 3; k++) len += (size_t)snprintf(text + len, cap - len, "%d ", rng_bool(&g) ? v[k] : -v[k]);
        len += (size_t)snprintf(text + len, cap - len, "0\n");
    }
    Cnf c = must_parse(text);
    free(text);
    return c;
}

/* ---- ranking invariant: neutral <=> zero breaks, prime <=> at least one --- */

static void test_ranking_invariant_neutral_means_zero_breaks(void) {
    Cnf c = random_cnf(80, 300, 99);
    Solver *s = must_solver(&c, 5);
    Rng rng = rng_new(4242);
    for (int round = 0; round < 40; round++) {
        for (int i = 0; i < 7; i++) solver_flip(s, rng_index(&rng, c.n_vars));
        const VarStats *st = solver_var_stats(s);
        for (size_t v = 0; v < c.n_vars; v++) {
            /* independent recount: clauses where v's literal is true and is the only true literal */
            size_t breaks = 0;
            for (size_t i = 0, n = solver_occ_count(s, v); i < n; i++) {
                size_t ci = solver_occ_clause(s, v, i);
                const Lit *cl = cnf_clause(&c, ci);
                for (size_t k = 0; k < cnf_clause_len(&c, ci); k++)
                    if (cl[k].var == v && lit_value(cl[k], solver_assignment(s)[v]) && solver_clause_support(s, ci) == 1) breaks++;
            }
            CHECK(st[v].break_count == breaks);
            CHECK((st[v].state == VAR_PRIME) == (breaks >= 1));
            if (st[v].state == VAR_NEUTRAL) CHECK(st[v].break_count == 0);
            CHECK(solver_refresh_var(s, v).state == st[v].state);   /* single-var refresh agrees */
        }
    }
    solver_free(s); cnf_free(&c);
}

/* ---- groups ------------------------------------------------------------- */

/* Everything a GroupResult reports must be recomputable from its assignment. */
static void check_group_result(const Cnf *c, const GroupResult *r) {
    size_t nv = c->n_vars, nc = c->n_clauses;
    unsigned char *is_rogue = calloc(nc ? nc : 1, 1), *in_rogue = calloc(nc ? nc : 1, 1);
    size_t unsat = 0;
    for (size_t ci = 0; ci < nc; ci++) {
        bool any = false;
        for (size_t k = 0; k < cnf_clause_len(c, ci); k++) {
            Lit l = cnf_clause(c, ci)[k];
            any |= lit_value(l, r->assignment[l.var]);
        }
        if (!any) { is_rogue[ci] = 1; unsat++; }
    }
    CHECK(r->n_rogue == unsat);
    for (size_t i = 0; i < r->n_rogue; i++) { CHECK(r->rogue[i] < nc && is_rogue[r->rogue[i]]); in_rogue[r->rogue[i]] = 1; }
    for (size_t ci = 0; ci < nc; ci++) CHECK(is_rogue[ci] == in_rogue[ci]);
    CHECK((r->status == SOLVE_SAT) == (unsat == 0));
    if (r->status == SOLVE_SAT) CHECK(verify(c, r->assignment, nv));

    Solver *ref = must_solver(c, 1);
    solver_set_assignment(ref, r->assignment);
    const VarStats *st = solver_var_stats(ref);
    unsigned char *neu = calloc(nv ? nv : 1, 1), *pri = calloc(nv ? nv : 1, 1);
    for (size_t i = 0; i < r->n_neutral; i++) neu[r->neutral[i]] = 1;
    for (size_t i = 0; i < r->n_prime; i++) pri[r->prime[i]] = 1;
    for (size_t v = 0; v < nv; v++) {
        CHECK(neu[v] == (st[v].state == VAR_NEUTRAL));
        CHECK(pri[v] == (st[v].state == VAR_PRIME));
        CHECK(!(neu[v] && pri[v]));
    }
    solver_free(ref);

    /* group clause lists: consistent with group_sizes, ids valid */
    if (r->status != SOLVE_UNSAT) {
        CHECK(r->group_off[0] == 0);
        for (size_t k = 0; k < r->n_groups; k++) {
            CHECK(r->group_off[k + 1] - r->group_off[k] == r->group_sizes[k]);
            for (size_t j = r->group_off[k]; j < r->group_off[k + 1]; j++) CHECK(r->group_clauses[j] < nc);
        }
        /* unmerged_groups: exactly the groups containing each unmerged clause, ascending */
        size_t *ridx = malloc((nc ? nc : 1) * sizeof *ridx);
        for (size_t ci = 0; ci < nc; ci++) ridx[ci] = SIZE_MAX;
        for (size_t i = 0; i < r->n_rogue; i++) ridx[r->rogue[i]] = i;
        size_t listed = 0, expected = 0;
        for (size_t i = 0; i < r->n_rogue; i++) {
            CHECK(r->rogue_group_off[i + 1] >= r->rogue_group_off[i]);
            for (size_t j = r->rogue_group_off[i]; j < r->rogue_group_off[i + 1]; j++) {
                size_t g = r->rogue_group[j];
                CHECK(g < r->n_groups);
                CHECK(j == r->rogue_group_off[i] || r->rogue_group[j - 1] < g);   /* strictly ascending */
                bool found = false;
                for (size_t q = r->group_off[g]; q < r->group_off[g + 1]; q++) found |= r->group_clauses[q] == r->rogue[i];
                CHECK(found);                                                      /* it really was in that group */
                listed++;
            }
        }
        for (size_t k = 0; k < r->n_groups; k++)                                   /* and no membership is missing */
            for (size_t j = r->group_off[k]; j < r->group_off[k + 1]; j++) {
                size_t ri = ridx[r->group_clauses[j]];
                if (ri == SIZE_MAX) continue;
                expected++;
                bool listed_here = false;
                for (size_t q = r->rogue_group_off[ri]; q < r->rogue_group_off[ri + 1]; q++) listed_here |= r->rogue_group[q] == k;
                CHECK(listed_here);
            }
        CHECK(listed == expected);
        free(ridx);
    }

    CHECK(r->merge_flips <= nv);                     /* each variable fires at most once */
    CHECK(r->merge_flips_free <= r->merge_flips);
    if (!r->merged) CHECK(r->merge_flips == 0);
    free(is_rogue); free(in_rogue); free(neu); free(pri);
}

static bool same_group_result(const GroupResult *a, const GroupResult *b) {
    return a->status == b->status && a->n_groups == b->n_groups && a->steps == b->steps &&
           a->merge_flips == b->merge_flips && a->n_rogue == b->n_rogue &&
           memcmp(a->assignment, b->assignment, a->n_vars) == 0;
}

static void test_groups_small_and_edge_cases(void) {
    SolverConfig cfg = solver_config_default();
    GroupConfig gc = group_config_default();

    ResultCnf rc = cnf_from_dimacs_file("tests/fixtures/small_sat.cnf");
    CHECK(rc.ok);
    if (rc.ok) {
        ResultGroup r = group_solve(&rc.val, &cfg, &gc);
        CHECK(r.ok && r.val.status == SOLVE_SAT);
        if (r.ok) { check_group_result(&rc.val, &r.val); group_result_free(&r.val); }
        cnf_free(&rc.val);
    }

    Cnf u = must_parse("p cnf 3 3\n1 2 0\n1 0\n-1 0\n");   /* structural pre-check still applies */
    ResultGroup r = group_solve(&u, &cfg, &gc);
    CHECK(r.ok && r.val.status == SOLVE_UNSAT && r.val.n_contradictions == 1);
    if (r.ok) group_result_free(&r.val);
    cnf_free(&u);

    Cnf e = must_parse("p cnf 2 2\n1 2 0\n0\n");           /* empty clause: no group can solve it */
    r = group_solve(&e, &cfg, &gc);
    CHECK(r.ok && r.val.status == SOLVE_UNKNOWN && r.val.n_groups == 0 && !r.val.merged);
    if (r.ok) { CHECK(r.val.n_rogue == 1 && r.val.rogue[0] == 1); check_group_result(&e, &r.val); group_result_free(&r.val); }
    cnf_free(&e);

    Cnf z = must_parse("p cnf 4 0\n");                      /* no clauses at all */
    r = group_solve(&z, &cfg, &gc);
    CHECK(r.ok && r.val.status == SOLVE_SAT && r.val.n_groups == 0);
    if (r.ok) group_result_free(&r.val);
    cnf_free(&z);
}

static void test_groups_contracts_on_random_instances(void) {
    SolverConfig cfg = solver_config_default();
    cfg.max_steps = 5000; cfg.restarts = 2;
    size_t merged_runs = 0, merged_sat = 0, directed_flips = 0, converged_sat = 0;
    for (int i = 0; i < 80; i++) {
        int n = 20 + (i % 4) * 15;
        double ratio = 2.0 + 0.25 * (i % 5);
        Cnf c = random_cnf(n, (int)(n * ratio), 1000 + (uint64_t)i);
        GroupConfig gc = group_config_default();
        gc.max_groups = 64;
        gc.diversify = (i % 3 == 0) ? 5 : 0;
        gc.directed_only = (i % 7 == 0);

        ResultGroup a = group_solve(&c, &cfg, &gc);
        ResultGroup b = group_solve(&c, &cfg, &gc);
        CHECK(a.ok && b.ok);
        if (!a.ok || !b.ok) { cnf_free(&c); continue; }
        check_group_result(&c, &a.val);
        CHECK(same_group_result(&a.val, &b.val));            /* fully deterministic for a given seed */
        CHECK(a.val.n_groups <= 64);
        if (gc.directed_only) CHECK(a.val.merge_flips_free == 0);
        if (a.val.merged) {
            merged_runs++;
            merged_sat += a.val.status == SOLVE_SAT;
            directed_flips += a.val.merge_flips - a.val.merge_flips_free;
        } else if (a.val.status == SOLVE_SAT) {
            converged_sat++;
        }
        group_result_free(&a.val); group_result_free(&b.val);
        cnf_free(&c);
    }
    /* sanity: all three paths (formation closes / merge completes / directed flips) really occur */
    CHECK(converged_sat > 0);
    CHECK(merged_runs > 0);
    CHECK(merged_sat > 0);
    CHECK(directed_flips > 0);
}

static void test_group_len_limits_group_size(void) {
    SolverConfig cfg = solver_config_default();
    cfg.max_steps = 5000; cfg.restarts = 2;
    size_t capped = 0;
    for (int i = 0; i < 30; i++) {
        Cnf c = random_cnf(150, 450, 7000 + (uint64_t)i);
        GroupConfig base = group_config_default();
        base.max_groups = 300;
        GroupResult none;                                    /* group_len = 0: ALL rogue clauses */
        ResultGroup r0 = group_solve(&c, &cfg, &base);
        CHECK(r0.ok);
        if (!r0.ok) { cnf_free(&c); continue; }
        none = r0.val;

        for (size_t L = 1; L <= 4; L += 3) {                 /* L = 1 and L = 4 */
            GroupConfig gc = base;
            gc.group_len = L;
            ResultGroup r = group_solve(&c, &cfg, &gc);
            CHECK(r.ok);
            if (!r.ok) continue;
            check_group_result(&c, &r.val);
            for (size_t k = 0; k < r.val.n_groups; k++) CHECK(r.val.group_sizes[k] >= 1 && r.val.group_sizes[k] <= L);
            /* the first group is cut from the same initial rogue set */
            if (none.n_groups > 0 && r.val.n_groups > 0)
                CHECK(r.val.group_sizes[0] == (none.group_sizes[0] < L ? none.group_sizes[0] : L));
            if (none.n_groups > 0 && none.group_sizes[0] > L) capped++;
            ResultGroup r2 = group_solve(&c, &cfg, &gc);     /* deterministic */
            CHECK(r2.ok && same_group_result(&r.val, &r2.val));
            if (r2.ok) group_result_free(&r2.val);
            group_result_free(&r.val);
        }

        GroupConfig big = base;                              /* a limit above every rogue set == no limit */
        big.group_len = 1000000;
        ResultGroup rb = group_solve(&c, &cfg, &big);
        CHECK(rb.ok && same_group_result(&none, &rb.val));
        if (rb.ok) group_result_free(&rb.val);
        group_result_free(&none);
        cnf_free(&c);
    }
    CHECK(capped > 0);   /* the limit really cut something */
}

/* More than 64 groups (the old membership-mask limit) must work end to end. */
static void test_more_than_64_groups(void) {
    Cnf c = random_cnf(1000, 3000, 31337);
    SolverConfig cfg = solver_config_default();
    cfg.max_steps = 5000; cfg.restarts = 2;
    GroupConfig gc = group_config_default();
    gc.max_groups = 400;
    gc.group_len = 10;
    ResultGroup r = group_solve(&c, &cfg, &gc);
    CHECK(r.ok);
    if (r.ok) {
        CHECK(r.val.n_groups > 64 && r.val.n_groups <= 400);
        for (size_t k = 0; k < r.val.n_groups; k++) CHECK(r.val.group_sizes[k] <= 10);
        check_group_result(&c, &r.val);
        group_result_free(&r.val);
    }
    cnf_free(&c);
}

/* ---- rarest-first constructive start ------------------------------------ */

static size_t brute_unsat(const Cnf *c, const bool *a) {
    size_t n = 0;
    for (size_t ci = 0; ci < c->n_clauses; ci++) {
        bool any = false;
        for (size_t k = 0; k < cnf_clause_len(c, ci); k++) any |= lit_value(cnf_clause(c, ci)[k], a[cnf_clause(c, ci)[k].var]);
        n += !any;
    }
    return n;
}

static void test_init_rarest_hand_checked(void) {
    /* Rarest by max(#x,#!x): x2 and x3 have 1, x1 has 2, so x2 goes first.
     * x2 false satisfies (-2 -3), which drops x3's negative occurrence; x3 then
     * takes (1 3) as an open clause and goes true. x1 last: both clauses forced. */
    Cnf c = must_parse("p cnf 3 3\n1 2 0\n1 3 0\n-2 -3 0\n");
    Solver *s = must_solver(&c, 1);
    CHECK(solver_init_rarest(s));
    const bool *a = solver_assignment(s);
    CHECK(a[0] && !a[1] && a[2]);
    CHECK(solver_unsatisfied_count(s) == 0);
    solver_free(s); cnf_free(&c);

    /* x2 occurs once, x1 twice. x2 goes first: (-1 2) open -> true. x1: unit (1) is forced -> true. */
    Cnf d = must_parse("p cnf 2 2\n1 0\n-1 2 0\n");
    s = must_solver(&d, 1);
    CHECK(solver_init_rarest(s));
    a = solver_assignment(s);
    CHECK(a[0] && a[1] && solver_unsatisfied_count(s) == 0);
    solver_free(s); cnf_free(&d);

    /* variable that occurs nowhere is set false */
    Cnf e = must_parse("p cnf 3 1\n1 2 0\n");
    s = must_solver(&e, 1);
    CHECK(solver_init_rarest(s));
    CHECK(!solver_assignment(s)[2]);
    solver_free(s); cnf_free(&e);
}

static void test_init_glowest_hand_checked(void) {
    /* Glowest by max(#x,#!x): x1 has 2, x2 and x3 have 1, so x1 goes first.
     * Both clauses are open and positive for x1, so it goes true and satisfies
     * them; x2 and x3 then have no open clause and tie on total polarity, so
     * both go false. */
    Cnf c = must_parse("p cnf 3 3\n1 2 0\n1 3 0\n-2 -3 0\n");
    Solver *s = must_solver(&c, 1);
    CHECK(solver_init_glowest(s));
    const bool *a = solver_assignment(s);
    CHECK(a[0] && !a[1] && !a[2]);
    CHECK(solver_unsatisfied_count(s) == 0);
    solver_free(s);

    /* solver_solve's restart-0 dispatch must take the same start. */
    Solver *d = must_solver(&c, 1);
    SolverConfig sc = solver_config_default();
    sc.init = INIT_GLOWEST;
    sc.max_steps = 0;
    sc.restarts = 0;
    ResultSolve r = solver_solve(d, &sc);
    const bool *da = solver_assignment(d);
    CHECK(r.ok && da[0] && !da[1] && !da[2]);
    if (r.ok) solve_result_free(&r.val);
    solver_free(d);

    SolverConfig cfg = solver_config_default();          /* and the group solver */
    cfg.init = INIT_GLOWEST;
    GroupConfig gc = group_config_default();
    gc.max_groups = 8;
    ResultGroup g = group_solve(&c, &cfg, &gc);
    CHECK(g.ok);
    if (g.ok) { check_group_result(&c, &g.val); group_result_free(&g.val); }
    cnf_free(&c);

    /* variable that occurs nowhere is set false */
    Cnf e = must_parse("p cnf 3 1\n1 2 0\n");
    s = must_solver(&e, 1);
    CHECK(solver_init_glowest(s));
    CHECK(!solver_assignment(s)[2]);
    solver_free(s); cnf_free(&e);
}

static void test_init_yanus(void) {
    Cnf c = must_parse("p cnf 4 4\n1 2 3 0\n-1 4 0\n-2 4 0\n-3 -4 0\n");
    SolverConfig cfg = solver_config_default();
    cfg.init = INIT_YANUS;
    cfg.yanus_attempts = 4;

    Solver *a = must_solver(&c, 12345), *b = must_solver(&c, 12345);
    CHECK(solver_init_yanus(a, &cfg));
    CHECK(solver_init_yanus(b, &cfg));
    CHECK(memcmp(solver_assignment(a), solver_assignment(b), c.n_vars) == 0);
    solver_free(a); solver_free(b);

    cfg.yanus_attempts = 0; cfg.yanus_timeout_ms = 0; /* no attempts: plain glowest */
    Solver *y = must_solver(&c, 999), *g = must_solver(&c, 999);
    CHECK(solver_init_yanus(y, &cfg));
    CHECK(solver_init_glowest(g));
    CHECK(memcmp(solver_assignment(y), solver_assignment(g), c.n_vars) == 0);
    solver_free(y); solver_free(g);

    cfg.yanus_timeout_ms = solver_config_default().yanus_timeout_ms;
    cfg.yanus_attempts = 2;   /* -saturate fixes D from the clause count */
    cfg.yanus_saturate = 0.5;
    Solver *p = must_solver(&c, 55), *q = must_solver(&c, 55);
    CHECK(solver_init_yanus(p, &cfg));
    CHECK(solver_init_yanus(q, &cfg));
    CHECK(memcmp(solver_assignment(p), solver_assignment(q), c.n_vars) == 0);
    solver_free(p); solver_free(q);
    cfg.yanus_saturate = 0.0;

    cfg.yanus_attempts = 2;   /* solver_solve dispatch */
    cfg.max_steps = 0;
    cfg.restarts = 0;
    Solver *d = must_solver(&c, 7);
    ResultSolve r = solver_solve(d, &cfg);
    CHECK(r.ok && r.val.status != SOLVE_UNSAT);
    if (r.ok && r.val.status == SOLVE_SAT) CHECK(verify(&c, r.val.assignment, c.n_vars));
    if (r.ok) solve_result_free(&r.val);
    solver_free(d);
    cnf_free(&c);
}

static void test_terminator_hand_checked(void) {
    /* Majority start leaves 1 unsat; terminator hill-climbs but may not solve. */
    Cnf c = must_parse("p cnf 3 3\n1 2 0\n-1 2 0\n-2 3 0\n");
    Solver *s = must_solver(&c, 1);
    SolverConfig cfg = solver_config_default();
    cfg.init = INIT_TERMINATOR;
    cfg.terminator_rounds = 16;
    cfg.terminator_order = TERM_FORWARD;
    cfg.max_steps = 0; cfg.restarts = 0;
    ResultSolve r = solver_solve(s, &cfg);
    CHECK(r.ok);
    if (r.ok) {
        if (r.val.status == SOLVE_SAT) CHECK(verify(&c, r.val.assignment, r.val.n_vars));
        solve_result_free(&r.val);
    }
    /* assignment is valid (all vars set) */
    CHECK(solver_unsatisfied_count(s) <= 3);
    solver_free(s); cnf_free(&c);
}

static void test_terminator_properties(void) {
    Rng rng = rng_new(1234);
    for (int t = 0; t < 60; t++) {
        Cnf c = random_cnf(30, 120, rng_next_u64(&rng));
        SolverConfig cfg = solver_config_default();
        cfg.init = INIT_TERMINATOR;
        cfg.terminator_rounds = 16;
        cfg.max_steps = 0; cfg.restarts = 0;
        Solver *s = must_solver(&c, 1);
        ResultSolve r = solver_solve(s, &cfg);
        CHECK(r.ok);
        if (r.ok) {
            if (r.val.status == SOLVE_SAT) {
                CHECK(verify(&c, r.val.assignment, r.val.n_vars));
                CHECK(solver_unsatisfied_count(s) == 0);
            }
            solve_result_free(&r.val);
        }
        solver_free(s); cnf_free(&c);
    }
}

static void test_glowest_likho_terminator_solve(void) {
    Rng rng = rng_new(777);
    for (int t = 0; t < 40; t++) {
        Cnf c = random_cnf(25, 100, rng_next_u64(&rng));
        InitMode modes[] = { INIT_GLOWEST, INIT_LIKHO, INIT_TERMINATOR };
        for (int mi = 0; mi < 3; mi++) {
            SolverConfig cfg = solver_config_default();
            cfg.init = modes[mi];
            cfg.max_steps = 2000;
            cfg.restarts = 5;
            Solver *s = must_solver(&c, 1);
            ResultSolve r = solver_solve(s, &cfg);
            CHECK(r.ok);
            if (r.ok) { if (r.val.status == SOLVE_SAT) { CHECK(verify(&c, r.val.assignment, r.val.n_vars)); CHECK(solver_unsatisfied_count(s) == 0); } solve_result_free(&r.val); }
            solver_free(s);
        }
        cnf_free(&c);
    }
}

static void test_init_min_overlap_hand_checked(void) {
    /* Step 0 is glowest's pick: x4 and x5 both have max(#x,#!x)=3, so x4 (lower
     * id) goes first and, all-negative, goes false. That satisfies clauses 1, 4
     * and 5. The least-overlapping unset variable is then x1 (0 of 1
     * occurrences already satisfied), not x5 (3 of 5), so x1 goes true. Next x2
     * and x6 tie at 1 of 2 and x2 wins by id; it goes true. x5 (5/5) and x6
     * (2/2) follow, both false. Glowest instead takes x5 at step 1 and ends with
     * x2 false, so this pins the order. */
    Cnf c = must_parse("p cnf 6 5\n"
                       "5 -4 -2 0\n"
                       "-5 2 6 0\n"
                       "-5 1 0\n"
                       "-4 -5 0\n"
                       "-6 5 -4 0\n");
    Solver *s = must_solver(&c, 1);
    CHECK(solver_init_min_overlap(s));
    const bool *a = solver_assignment(s);
    CHECK(a[0] && a[1] && !a[2] && !a[3] && !a[4] && !a[5]);
    CHECK(solver_unsatisfied_count(s) == 0);
    solver_free(s);

    Solver *g = must_solver(&c, 1);                      /* glowest's order differs */
    CHECK(solver_init_glowest(g));
    CHECK(!solver_assignment(g)[1]);
    solver_free(g);

    /* solver_solve's restart-0 dispatch must take the same start. */
    Solver *d = must_solver(&c, 1);
    SolverConfig sc = solver_config_default();
    sc.init = INIT_MIN_OVERLAP;
    sc.max_steps = 0;
    sc.restarts = 0;
    ResultSolve r = solver_solve(d, &sc);
    const bool *da = solver_assignment(d);
    CHECK(r.ok && da[0] && da[1] && !da[2] && !da[3] && !da[4] && !da[5]);
    if (r.ok) solve_result_free(&r.val);
    solver_free(d);

    SolverConfig cfg = solver_config_default();          /* and the group solver */
    cfg.init = INIT_MIN_OVERLAP;
    GroupConfig gc = group_config_default();
    gc.max_groups = 8;
    ResultGroup gr = group_solve(&c, &cfg, &gc);
    CHECK(gr.ok);
    if (gr.ok) { check_group_result(&c, &gr.val); group_result_free(&gr.val); }
    cnf_free(&c);
}

static void test_init_gob_hand_checked(void) {
    /* Units pin the decode: 1 is +x = 1, -2 is -x = 1 (negative, so false), 3 is
     * +x = 1, so the start is exactly (true, false, true). */
    Cnf c = must_parse("p cnf 3 3\n1 0\n-2 0\n3 0\n");
    Solver *s = must_solver(&c, 1);
    CHECK(solver_init_gob(s));
    const bool *a = solver_assignment(s);
    CHECK(a[0] && !a[1] && a[2]);
    CHECK(solver_unsatisfied_count(s) == 0);
    solver_free(s); cnf_free(&c);

    /* A single clause 1 2 projects to (1/2, 1/2): both positive, so it holds. */
    Cnf d = must_parse("p cnf 2 1\n1 2 0\n");
    s = must_solver(&d, 1);
    CHECK(solver_init_gob(s));
    a = solver_assignment(s);
    CHECK(a[0] && a[1] && solver_unsatisfied_count(s) == 0);
    solver_free(s); cnf_free(&d);

    /* Mixed signs: (1 -2) projects to (1/2, -1/2); -1/2 shifts to +1/2, so both
     * decode true and the clause holds. */
    Cnf e = must_parse("p cnf 2 1\n1 -2 0\n");
    s = must_solver(&e, 1);
    CHECK(solver_init_gob(s));
    a = solver_assignment(s);
    CHECK(a[0] && a[1] && solver_unsatisfied_count(s) == 0);
    solver_free(s); cnf_free(&e);

    /* Empty clause and empty formula: no crash, the empty clause stays rogue. */
    Cnf f = must_parse("p cnf 1 1\n0\n");
    s = must_solver(&f, 1);
    CHECK(solver_init_gob(s));
    CHECK(solver_unsatisfied_count(s) == 1);
    solver_free(s); cnf_free(&f);

    Cnf g = must_parse("p cnf 0 0\n");
    s = must_solver(&g, 1);
    CHECK(solver_init_gob(s));
    CHECK(solver_unsatisfied_count(s) == 0);
    solver_free(s); cnf_free(&g);
}

static void test_init_gob_sign_hand_checked(void) {
    /* Units: 1 is +x = 1, -2 is -x = 1 (negative, so false), 3 is +x = 1. */
    Cnf c = must_parse("p cnf 3 3\n1 0\n-2 0\n3 0\n");
    Solver *s = must_solver(&c, 1);
    CHECK(solver_init_gob_sign(s));
    const bool *a = solver_assignment(s);
    CHECK(a[0] && !a[1] && a[2]);
    CHECK(solver_unsatisfied_count(s) == 0);
    solver_free(s); cnf_free(&c);

    /* Mixed signs: (1 -2) projects to (1/2, -1/2); the sign decode makes the
     * second bit false, where gob's magnitude decode makes it true. The clause
     * holds on the first literal. */
    Cnf e = must_parse("p cnf 2 1\n1 -2 0\n");
    s = must_solver(&e, 1);
    CHECK(solver_init_gob_sign(s));
    a = solver_assignment(s);
    CHECK(a[0] && !a[1] && solver_unsatisfied_count(s) == 0);
    solver_free(s); cnf_free(&e);

    /* Negative clause (-1 -2) projects to (-1/2, -1/2): both bits false, which
     * satisfies the clause. gob's magnitude decode makes both true and has to
     * repair it, so this pins the sign rule for negative fractions. */
    Cnf k = must_parse("p cnf 2 1\n-1 -2 0\n");
    s = must_solver(&k, 1);
    CHECK(solver_init_gob_sign(s));
    a = solver_assignment(s);
    CHECK(!a[0] && !a[1] && solver_unsatisfied_count(s) == 0);
    solver_free(s); cnf_free(&k);

    /* Empty clause and empty formula: no crash; the empty clause stays rogue. */
    Cnf f = must_parse("p cnf 1 1\n0\n");
    s = must_solver(&f, 1);
    CHECK(solver_init_gob_sign(s));
    CHECK(solver_unsatisfied_count(s) == 1);
    solver_free(s); cnf_free(&f);

    Cnf g = must_parse("p cnf 0 0\n");
    s = must_solver(&g, 1);
    CHECK(solver_init_gob_sign(s));
    CHECK(solver_unsatisfied_count(s) == 0);
    solver_free(s); cnf_free(&g);
}

static void test_init_gob_square_hand_checked(void) {
    /* Square system == the whole system here (n == m); the sign decode gives the
     * same (true, false, true) as gob on these units. Phase 3 has no other rows. */
    Cnf c = must_parse("p cnf 3 3\n1 0\n-2 0\n3 0\n");
    Solver *s = must_solver(&c, 1);
    GobSquareCheck chk;
    CHECK(solver_init_gob_square(s, &chk));
    const bool *a = solver_assignment(s);
    CHECK(a[0] && !a[1] && a[2]);
    CHECK(solver_unsatisfied_count(s) == 0);
    CHECK(chk.selected_bad == 0);
    CHECK(chk.other_total == 0 && chk.other_bad == 0);
    solver_free(s); cnf_free(&c);

    /* Same square case as gob: (1 -2) decodes (true, false) by sign. */
    Cnf m = must_parse("p cnf 2 1\n1 -2 0\n");
    s = must_solver(&m, 1);
    CHECK(solver_init_gob_square(s, &chk));
    a = solver_assignment(s);
    CHECK(a[0] && !a[1] && solver_unsatisfied_count(s) == 0);
    solver_free(s); cnf_free(&m);

    /* m > n: exactly n rows are selected, the other m - n are reported on. */
    Cnf d = must_parse("p cnf 2 3\n1 2 0\n1 -2 0\n-1 2 0\n");
    s = must_solver(&d, 1);
    CHECK(solver_init_gob_square(s, &chk));
    /* The last two rows are async twins, so only two equations reach square selection. */
    CHECK(chk.other_total == 0);
    CHECK(chk.other_bad <= chk.other_total);
    CHECK(solver_unsatisfied_count(s) == brute_unsat(&d, solver_assignment(s)));
    solver_free(s); cnf_free(&d);

    /* Empty clause and empty formula: no crash; the empty clause stays rogue. */
    Cnf f = must_parse("p cnf 1 1\n0\n");
    s = must_solver(&f, 1);
    CHECK(solver_init_gob_square(s, NULL));                /* the check is optional */
    CHECK(solver_unsatisfied_count(s) == 1);
    solver_free(s); cnf_free(&f);

    Cnf g = must_parse("p cnf 0 0\n");
    s = must_solver(&g, 1);
    CHECK(solver_init_gob_square(s, &chk));
    CHECK(solver_unsatisfied_count(s) == 0);
    CHECK(chk.selected_bad == 0 && chk.other_bad == 0);
    solver_free(s); cnf_free(&g);
}

static void test_init_gob_square_properties(void) {
    SolverConfig cfg = solver_config_default();
    cfg.init = INIT_GOB_SQUARE;
    cfg.max_steps = 20000; cfg.restarts = 2;
    for (int i = 0; i < 25; i++) {
        int n = 60 + 20 * (i % 4);
        Cnf c = random_cnf(n, (int)(n * (2.0 + 0.4 * (i % 5))), 1000 + (uint64_t)i);
        Solver *a = must_solver(&c, 1), *b = must_solver(&c, 99);   /* different RNG seeds: no randomness used */
        GobSquareCheck ca, cb;
        CHECK(solver_init_gob_square(a, &ca) && solver_init_gob_square(b, &cb));
        CHECK(memcmp(solver_assignment(a), solver_assignment(b), c.n_vars) == 0);   /* deterministic */
        CHECK(ca.selected_bad == cb.selected_bad && ca.other_bad == cb.other_bad);  /* check too */
        CHECK(solver_unsatisfied_count(a) == brute_unsat(&c, solver_assignment(a)));/* counters consistent */
        CHECK(ca.selected_bad + ca.other_bad <= c.n_clauses);
        CHECK(ca.other_total <= c.n_clauses);
        solver_free(a); solver_free(b);

        Solver *s = must_solver(&c, 5);                       /* plain solver from the square start */
        ResultSolve r = solver_solve(s, &cfg);
        CHECK(r.ok);
        if (r.ok && r.val.status == SOLVE_SAT) CHECK(verify(&c, r.val.assignment, r.val.n_vars));
        if (r.ok) solve_result_free(&r.val);
        solver_free(s);

        GroupConfig gc = group_config_default();               /* and the group solver */
        gc.max_groups = 64;
        ResultGroup g = group_solve(&c, &cfg, &gc);
        CHECK(g.ok);
        if (g.ok) { check_group_result(&c, &g.val); group_result_free(&g.val); }
        cnf_free(&c);
    }
}

static void test_init_gob_properties(void) {
    SolverConfig cfg = solver_config_default();
    cfg.init = INIT_GOB;
    cfg.max_steps = 20000; cfg.restarts = 2;
    for (int i = 0; i < 25; i++) {
        int n = 60 + 20 * (i % 4);
        Cnf c = random_cnf(n, (int)(n * (2.0 + 0.4 * (i % 5))), 900 + (uint64_t)i);
        Solver *a = must_solver(&c, 1), *b = must_solver(&c, 99);   /* different RNG seeds: no randomness used */
        CHECK(solver_init_gob(a) && solver_init_gob(b));
        CHECK(memcmp(solver_assignment(a), solver_assignment(b), c.n_vars) == 0);   /* deterministic */
        CHECK(solver_unsatisfied_count(a) == brute_unsat(&c, solver_assignment(a))); /* counters consistent */
        solver_free(a); solver_free(b);

        Solver *s = must_solver(&c, 5);                       /* plain solver from the gob start */
        ResultSolve r = solver_solve(s, &cfg);
        CHECK(r.ok);
        if (r.ok && r.val.status == SOLVE_SAT) CHECK(verify(&c, r.val.assignment, r.val.n_vars));
        if (r.ok) solve_result_free(&r.val);
        solver_free(s);

        GroupConfig gc = group_config_default();               /* and the group solver */
        gc.max_groups = 64;
        ResultGroup g = group_solve(&c, &cfg, &gc);
        CHECK(g.ok);
        if (g.ok) { check_group_result(&c, &g.val); group_result_free(&g.val); }
        cnf_free(&c);
    }
}

static void test_init_gob_sign_properties(void) {
    SolverConfig cfg = solver_config_default();
    cfg.init = INIT_GOB_SIGN;
    cfg.max_steps = 20000; cfg.restarts = 2;
    for (int i = 0; i < 25; i++) {
        int n = 60 + 20 * (i % 4);
        Cnf c = random_cnf(n, (int)(n * (2.0 + 0.4 * (i % 5))), 1100 + (uint64_t)i);
        Solver *a = must_solver(&c, 1), *b = must_solver(&c, 99);   /* different RNG seeds: no randomness used */
        CHECK(solver_init_gob_sign(a) && solver_init_gob_sign(b));
        CHECK(memcmp(solver_assignment(a), solver_assignment(b), c.n_vars) == 0);   /* deterministic */
        CHECK(solver_unsatisfied_count(a) == brute_unsat(&c, solver_assignment(a))); /* counters consistent */
        solver_free(a); solver_free(b);

        Solver *s = must_solver(&c, 5);                       /* plain solver from the sign start */
        ResultSolve r = solver_solve(s, &cfg);
        CHECK(r.ok);
        if (r.ok && r.val.status == SOLVE_SAT) CHECK(verify(&c, r.val.assignment, r.val.n_vars));
        if (r.ok) solve_result_free(&r.val);
        solver_free(s);

        GroupConfig gc = group_config_default();               /* and the group solver */
        gc.max_groups = 64;
        ResultGroup g = group_solve(&c, &cfg, &gc);
        CHECK(g.ok);
        if (g.ok) { check_group_result(&c, &g.val); group_result_free(&g.val); }
        cnf_free(&c);
    }
}

static void test_init_rarest_properties(void) {
    SolverConfig cfg = solver_config_default();
    cfg.init = INIT_RAREST;
    cfg.max_steps = 20000; cfg.restarts = 2;
    for (int i = 0; i < 25; i++) {
        Cnf c = random_cnf(60 + 20 * (i % 4), (int)((60 + 20 * (i % 4)) * (2.0 + 0.4 * (i % 5))), 500 + (uint64_t)i);
        Solver *a = must_solver(&c, 1), *b = must_solver(&c, 99);   /* different RNG seeds: no randomness used */
        CHECK(solver_init_rarest(a) && solver_init_rarest(b));
        CHECK(memcmp(solver_assignment(a), solver_assignment(b), c.n_vars) == 0);   /* deterministic */
        CHECK(solver_unsatisfied_count(a) == brute_unsat(&c, solver_assignment(a))); /* counters consistent */
        solver_free(a); solver_free(b);

        Solver *s = must_solver(&c, 5);                       /* plain solver started from the rarest-first state */
        ResultSolve r = solver_solve(s, &cfg);
        CHECK(r.ok);
        if (r.ok && r.val.status == SOLVE_SAT) CHECK(verify(&c, r.val.assignment, r.val.n_vars));
        if (r.ok) solve_result_free(&r.val);
        solver_free(s);

        GroupConfig gc = group_config_default();               /* and the group solver */
        gc.max_groups = 64;
        ResultGroup g = group_solve(&c, &cfg, &gc);
        CHECK(g.ok);
        if (g.ok) { check_group_result(&c, &g.val); group_result_free(&g.val); }
        cnf_free(&c);
    }
}

/* ---- Likho: zero start + neutral/switch-domain repair ------------------- */

/* Likho's phase 1: every variable false. */
static void init_zero(Solver *s, size_t nv) {
    bool *z = calloc(nv ? nv : 1, sizeof *z);
    solver_set_assignment(s, z);
    free(z);
}

static void test_likho_neutral_repair(void) {
    /* Zero start leaves the all-positive clause (1 2 3) rogue. Each of its
     * literals sits in a two-negative clause, so every variable is neutral and
     * flipping one satisfies the rogue clause and breaks nothing. */
    Cnf c = must_parse("p cnf 4 7\n"
                       "1 2 3 0\n"
                       "-1 -2 4 0\n-1 -3 4 0\n-2 -1 4 0\n"
                       "-2 -3 4 0\n-3 -1 4 0\n-3 -2 4 0\n");
    Solver *s = must_solver(&c, 1);
    init_zero(s, c.n_vars);
    CHECK(solver_unsatisfied_count(s) == 1);
    for (size_t k = 0; k < 3; k++)
        CHECK(solver_refresh_var(s, k).state == VAR_NEUTRAL);
    CHECK(solver_likho(s, NULL));
    CHECK(solver_unsatisfied_count(s) == 0);
    CHECK(verify(&c, solver_assignment(s), c.n_vars));
    solver_free(s); cnf_free(&c);
}

static void test_likho_domain_repair(void) {
    /* Zero start leaves (1 2 3) rogue. All three literals are primes, so no
     * neutral is available: x1's flip breaks (-1 4 5), repaired by the neutral
     * x4, giving the two-variable domain {x1, x4}. */
    Cnf c = must_parse("p cnf 7 5\n"
                       "1 2 3 0\n"
                       "-1 4 5 0\n"
                       "-2 6 7 0\n"
                       "-3 6 7 0\n"
                       "-4 -5 6 0\n");
    Solver *s = must_solver(&c, 1);
    init_zero(s, c.n_vars);
    CHECK(solver_unsatisfied_count(s) == 1);
    for (size_t k = 0; k < 3; k++)
        CHECK(solver_refresh_var(s, k).state == VAR_PRIME);
    CHECK(solver_refresh_var(s, 3).state == VAR_NEUTRAL);  /* x4 is a free repair */
    CHECK(solver_likho(s, NULL));
    CHECK(solver_unsatisfied_count(s) == 0);
    CHECK(verify(&c, solver_assignment(s), c.n_vars));
    /* the accepted domain was {x1, x4}: each fired exactly once */
    const bool *a = solver_assignment(s);
    for (size_t v = 0; v < c.n_vars; v++)
        CHECK(a[v] == (v == 0 || v == 3));
    solver_free(s); cnf_free(&c);
}

static void test_likho_switch_domain_chain(void) {
    /* Zero start leaves (1 2 3) rogue with all three literals primes. x1's
     * flip breaks (-1 2 3), whose false literals are primes too, so the only
     * repair chains x1 -> x2 -> x4, where x4 is neutral. */
    Cnf c = must_parse("p cnf 7 5\n"
                       "1 2 3 0\n"
                       "-1 2 3 0\n"
                       "-2 4 5 0\n"
                       "-3 6 7 0\n"
                       "-4 -6 7 0\n");
    Solver *s = must_solver(&c, 1);
    init_zero(s, c.n_vars);
    CHECK(solver_unsatisfied_count(s) == 1);
    CHECK(solver_refresh_var(s, 0).state == VAR_PRIME);    /* x1 breaks (-1 2 3) */
    CHECK(solver_refresh_var(s, 1).state == VAR_PRIME);    /* x2 breaks (-2 4 5) */
    CHECK(solver_refresh_var(s, 3).state == VAR_NEUTRAL);  /* x4 is a free repair */
    CHECK(solver_likho(s, NULL));
    CHECK(solver_unsatisfied_count(s) == 0);
    CHECK(verify(&c, solver_assignment(s), c.n_vars));
    /* the accepted domain was {x1, x2, x4}: each fired exactly once */
    const bool *a = solver_assignment(s);
    for (size_t v = 0; v < c.n_vars; v++)
        CHECK(a[v] == (v == 0 || v == 1 || v == 3));
    solver_free(s); cnf_free(&c);
}

static void test_likho_deep_dive_domains_first(void) {
    /* Zero start leaves (1 2 3) rogue. Neutrals first would flip the neutral
     * x2; domains first must instead fire x1's domain {x1, x4}. */
    Cnf c = must_parse("p cnf 5 3\n"
                       "1 2 3 0\n"
                       "-1 4 5 0\n"
                       "-2 -4 3 0\n");
    SolverConfig cfg = solver_config_default();
    cfg.likho_domains_first = true;
    Solver *s = must_solver(&c, 1);
    CHECK(solver_likho(s, &cfg));
    CHECK(solver_unsatisfied_count(s) == 0);
    CHECK(verify(&c, solver_assignment(s), c.n_vars));
    const bool *a = solver_assignment(s);
    for (size_t v = 0; v < c.n_vars; v++) CHECK(a[v] == (v == 0 || v == 3));
    solver_free(s);

    /* With the domain capped at one variable x1 has no repair, so the same
     * deep-dive falls back to the neutral x2. */
    cfg.likho_domain_max = 1;
    s = must_solver(&c, 1);
    CHECK(solver_likho(s, &cfg));
    CHECK(solver_unsatisfied_count(s) == 0);
    CHECK(verify(&c, solver_assignment(s), c.n_vars));
    a = solver_assignment(s);
    for (size_t v = 0; v < c.n_vars; v++) CHECK(a[v] == (v == 1));
    solver_free(s);

    /* Same fallback when the node budget is too small to close x1's domain. */
    cfg.likho_domain_max = 4;
    cfg.likho_search_nodes = 1;
    s = must_solver(&c, 1);
    CHECK(solver_likho(s, &cfg));
    CHECK(solver_unsatisfied_count(s) == 0);
    a = solver_assignment(s);
    for (size_t v = 0; v < c.n_vars; v++) CHECK(a[v] == (v == 1));
    solver_free(s); cnf_free(&c);
}

static void test_likho_pre_cook(void) {
    /* The pre-cook assignment is Likho's phase 1. On this formula all three
     * pre-cooks already solve it, so the repair must leave the pre-cooked
     * assignment untouched. */
    Cnf c = must_parse("p cnf 5 3\n"
                       "1 2 3 0\n"
                       "-1 4 5 0\n"
                       "-2 -4 3 0\n");
    SolverConfig cfg = solver_config_default();
    cfg.likho_pre_cook = INIT_MAJORITY;
    Solver *m = must_solver(&c, 1), *s = must_solver(&c, 1);
    solver_init_majority(m);
    CHECK(solver_unsatisfied_count(m) == 0);
    CHECK(solver_likho(s, &cfg));
    CHECK(memcmp(solver_assignment(s), solver_assignment(m), c.n_vars) == 0);
    solver_free(m); solver_free(s);

    cfg.likho_pre_cook = INIT_RAREST;
    m = must_solver(&c, 1); s = must_solver(&c, 1);
    CHECK(solver_init_rarest(m));
    CHECK(solver_unsatisfied_count(m) == 0);
    CHECK(solver_likho(s, &cfg));
    CHECK(memcmp(solver_assignment(s), solver_assignment(m), c.n_vars) == 0);
    solver_free(m); solver_free(s);

    cfg.likho_pre_cook = INIT_GLOWEST;
    m = must_solver(&c, 1); s = must_solver(&c, 1);
    CHECK(solver_init_glowest(m));
    CHECK(solver_unsatisfied_count(m) == 0);
    CHECK(solver_likho(s, &cfg));
    CHECK(memcmp(solver_assignment(s), solver_assignment(m), c.n_vars) == 0);
    solver_free(m); solver_free(s);

    cfg.likho_pre_cook = INIT_MIN_OVERLAP;
    m = must_solver(&c, 1); s = must_solver(&c, 1);
    CHECK(solver_init_min_overlap(m));
    CHECK(solver_unsatisfied_count(m) == 0);
    CHECK(solver_likho(s, &cfg));
    CHECK(memcmp(solver_assignment(s), solver_assignment(m), c.n_vars) == 0);
    solver_free(m); solver_free(s);

    cfg.likho_pre_cook = INIT_GOB;
    m = must_solver(&c, 1); s = must_solver(&c, 1);
    CHECK(solver_init_gob(m));
    CHECK(solver_unsatisfied_count(m) == 0);
    CHECK(solver_likho(s, &cfg));
    CHECK(memcmp(solver_assignment(s), solver_assignment(m), c.n_vars) == 0);
    solver_free(m); solver_free(s);

    cfg.likho_pre_cook = INIT_GOB_SIGN;
    m = must_solver(&c, 1); s = must_solver(&c, 1);
    CHECK(solver_init_gob_sign(m));
    CHECK(solver_unsatisfied_count(m) == 0);
    CHECK(solver_likho(s, &cfg));
    CHECK(memcmp(solver_assignment(s), solver_assignment(m), c.n_vars) == 0);
    solver_free(m); solver_free(s);

    cfg.likho_pre_cook = INIT_GOB_SQUARE;
    m = must_solver(&c, 1); s = must_solver(&c, 1);
    CHECK(solver_init_gob_square(m, NULL));
    CHECK(solver_unsatisfied_count(m) == 0);
    CHECK(solver_likho(s, &cfg));
    CHECK(memcmp(solver_assignment(s), solver_assignment(m), c.n_vars) == 0);
    solver_free(m); solver_free(s); cnf_free(&c);
}

static void test_likho_properties(void) {
    for (int i = 0; i < 25; i++) {
        Cnf c = random_cnf(60 + 20 * (i % 4), (int)((60 + 20 * (i % 4)) * (2.0 + 0.4 * (i % 5))), 700 + (uint64_t)i);
        Solver *base = must_solver(&c, 7), *a = must_solver(&c, 7), *b = must_solver(&c, 7);
        init_zero(base, c.n_vars);
        size_t before = solver_unsatisfied_count(base);
        CHECK(solver_likho(a, NULL) && solver_likho(b, NULL));
        CHECK(memcmp(solver_assignment(a), solver_assignment(b), c.n_vars) == 0);   /* same seed => same repair */
        CHECK(solver_unsatisfied_count(a) <= before);                               /* never worse than the zero start */
        CHECK(solver_unsatisfied_count(a) == brute_unsat(&c, solver_assignment(a)));/* counters consistent */
        solver_free(base); solver_free(a); solver_free(b);

        SolverConfig cfg = solver_config_default();
        cfg.init = INIT_LIKHO;
        cfg.max_steps = 20000; cfg.restarts = 2;
        Solver *s = must_solver(&c, 5);
        ResultSolve r = solver_solve(s, &cfg);
        CHECK(r.ok);
        if (r.ok && r.val.status == SOLVE_SAT) CHECK(verify(&c, r.val.assignment, r.val.n_vars));
        if (r.ok) solve_result_free(&r.val);
        solver_free(s);
        cnf_free(&c);
    }
}

/* ---- general-CNF generator + rarest reference (spec cross-check) ---------- */

/* Clauses of 1..maxlen literals over n variables (some variables may never occur). */
static Cnf random_general_cnf(int n, int m, int maxlen, uint64_t seed) {
    size_t cap = (size_t)m * (size_t)(maxlen * 12 + 4) + 64;
    char *text = malloc(cap);
    Rng g = rng_new(seed);
    size_t len = (size_t)snprintf(text, cap, "p cnf %d %d\n", n, m);
    for (int i = 0; i < m; i++) {
        int k = 1 + (int)rng_index(&g, (size_t)maxlen);
        for (int j = 0; j < k; j++) {
            int x = 1 + (int)rng_index(&g, (size_t)n);
            len += (size_t)snprintf(text + len, cap - len, "%d ", rng_bool(&g) ? x : -x);
        }
        len += (size_t)snprintf(text + len, cap - len, "0\n");
    }
    Cnf c = must_parse(text);
    free(text);
    return c;
}

/* Naive spec of the constructive starts: every step recomputes, for each unset
 * variable, the pick key from scratch. rarest/glowest use max(#x, #!x) over the
 * clauses not yet satisfied, smallest/largest first. min-overlap takes the
 * glowest pick first, then the smallest (#occurrences in satisfied clauses) /
 * (#occurrences), with a zero denominator last. Ties: lowest id. Then the value
 * rule forced > open > total polarity > false. No heap, no incremental counters. */
enum { REF_RAREST, REF_GLOWEST, REF_MIN_OVERLAP };

static void order_reference(const Cnf *c, bool *out, int mode) {
    size_t nv = c->n_vars, nc = c->n_clauses;
    bool *set = calloc(nv ? nv : 1, 1), *done = calloc(nc ? nc : 1, 1);
    for (size_t step = 0; step < nv; step++) {
        size_t best = SIZE_MAX, best_key = 0, best_den = 1;
        for (size_t v = 0; v < nv; v++) {
            if (set[v]) continue;
            size_t p = 0, n = 0, tot = 0, ov = 0;
            for (size_t ci = 0; ci < nc; ci++)
                for (size_t k = 0; k < cnf_clause_len(c, ci); k++) {
                    Lit l = cnf_clause(c, ci)[k];
                    if (l.var != v) continue;
                    tot++;
                    if (done[ci]) { ov++; continue; }
                    if (l.positive) p++; else n++;
                }
            bool overlap = mode == REF_MIN_OVERLAP && step > 0;
            size_t key = overlap ? ov : (p > n ? p : n), den = overlap ? tot : 1;
            bool better;
            if (overlap) {
                if (best == SIZE_MAX) better = true;
                else if ((den == 0) != (best_den == 0)) better = best_den == 0;
                else better = (unsigned __int128)key * best_den < (unsigned __int128)best_key * den;
            } else {
                bool most = mode == REF_GLOWEST || mode == REF_MIN_OVERLAP;   /* min-overlap's first pick is glowest's */
                better = best == SIZE_MAX || (most ? key > best_key : key < best_key);
            }
            if (better) { best = v; best_key = key; best_den = den; }
        }
        size_t v = best, fp = 0, fn = 0, op = 0, on = 0, tp = 0, tn = 0;
        for (size_t ci = 0; ci < nc; ci++) {
            size_t unset_here = 0;
            bool has = false, pos = false;
            for (size_t k = 0; k < cnf_clause_len(c, ci); k++) {
                Lit l = cnf_clause(c, ci)[k];
                if (!set[l.var]) unset_here++;
                if (l.var == v) { has = true; pos = l.positive; }
            }
            if (!has) continue;
            if (pos) tp++; else tn++;
            if (done[ci]) continue;
            if (unset_here == 1) { if (pos) fp++; else fn++; }
            else                 { if (pos) op++; else on++; }
        }
        bool val = (fp != fn) ? fp > fn : (op != on) ? op > on : (tp != tn) ? tp > tn : false;
        out[v] = val; set[v] = true;
        for (size_t ci = 0; ci < nc; ci++)
            for (size_t k = 0; k < cnf_clause_len(c, ci); k++) {
                Lit l = cnf_clause(c, ci)[k];
                if (l.var == v && l.positive == val) done[ci] = true;
            }
    }
    free(set); free(done);
}

static void check_order_matches_reference(const Cnf *c, int mode) {
    Solver *s = must_solver(c, 1);
    bool *ref = malloc(c->n_vars ? c->n_vars : 1);
    bool ok = mode == REF_RAREST ? solver_init_rarest(s)
            : mode == REF_GLOWEST ? solver_init_glowest(s)
            : solver_init_min_overlap(s);
    CHECK(ok);
    order_reference(c, ref, mode);
    CHECK(c->n_vars == 0 || memcmp(solver_assignment(s), ref, c->n_vars) == 0);
    CHECK(solver_unsatisfied_count(s) == brute_unsat(c, ref));
    free(ref);
    solver_free(s);
}

static void check_order_corpus(int mode) {
    for (int i = 0; i < 150; i++) {                           /* random 3-SAT, easy to hard */
        int n = 8 + (i % 7) * 9;
        Cnf c = random_cnf(n, (int)(n * (1.0 + 0.5 * (i % 9))), 9000 + (uint64_t)i);
        check_order_matches_reference(&c, mode);
        cnf_free(&c);
    }
    for (int i = 0; i < 150; i++) {                           /* general CNF: units, long clauses, unused vars */
        int n = 5 + (i % 6) * 7;
        Cnf c = random_general_cnf(n * 2, n + (i % 5) * 6, 1 + (i % 6), 4000 + (uint64_t)i);
        check_order_matches_reference(&c, mode);
        cnf_free(&c);
    }
    Cnf e = must_parse("p cnf 0 0\n");
    check_order_matches_reference(&e, mode);
    cnf_free(&e);
    Cnf one = must_parse("p cnf 1 1\n-1 0\n");
    check_order_matches_reference(&one, mode);
    cnf_free(&one);
}

static void test_init_rarest_matches_reference(void) { check_order_corpus(REF_RAREST); }
static void test_init_glowest_matches_reference(void) { check_order_corpus(REF_GLOWEST); }
static void test_init_min_overlap_matches_reference(void) { check_order_corpus(REF_MIN_OVERLAP); }

/* ---- Likho: properties the repair pass promises ---------------------------- */

/* After the pass, states are re-derived from the final assignment: the ranking
 * invariant must hold and no stale label may survive. */
static void test_likho_refreshed_states(void) {
    Cnf t = must_parse("p cnf 4 10\n1 2 3 0\n-1 -2 3 0\n-1 2 3 0\n-3 -1 2 0\n"
                       "-2 4 1 0\n-2 4 1 0\n-2 4 1 0\n-3 4 1 0\n-3 4 1 0\n-3 4 1 0\n");
    Solver *s = must_solver(&t, 1);
    CHECK(solver_likho(s, NULL) && solver_unsatisfied_count(s) == 0);
    const VarStats *st = solver_var_stats(s);
    for (size_t v = 0; v < t.n_vars; v++)
        CHECK((st[v].state == VAR_PRIME) == (st[v].break_count >= 1));
    solver_free(s); cnf_free(&t);
    for (int i = 0; i < 40; i++) {
        Cnf c = random_cnf(100, 330 + 6 * i, 5100 + (uint64_t)i);
        Solver *l = must_solver(&c, 3);
        CHECK(solver_likho(l, NULL));
        const VarStats *ls = solver_var_stats(l);
        for (size_t v = 0; v < c.n_vars; v++)
            CHECK((ls[v].state == VAR_PRIME) == (ls[v].break_count >= 1));   /* invariant holds after the pass */
        solver_free(l); cnf_free(&c);
    }
}

/* If a rogue clause contains a neutral variable after the zero start, one free
 * flip exists, so Likho must strictly reduce the rogue count; otherwise it must
 * never increase it. */
static void test_likho_strictly_improves_when_a_free_move_exists(void) {
    size_t with_free = 0;
    for (int i = 0; i < 80; i++) {
        int n = 60 + 20 * (i % 4);
        Cnf c = random_cnf(n, (int)(n * (2.0 + 0.4 * (i % 6))), 6100 + (uint64_t)i);
        Solver *m = must_solver(&c, 9), *l = must_solver(&c, 9);   /* same seed => same zero start */
        init_zero(m, c.n_vars);
        size_t before = solver_unsatisfied_count(m);
        bool free_move = false;
        const size_t *rl = solver_rogue_list(m);
        for (size_t r = 0; r < before; r++)
            for (size_t k = 0; k < cnf_clause_len(&c, rl[r]); k++)
                if (solver_refresh_var(m, cnf_clause(&c, rl[r])[k].var).state == VAR_NEUTRAL) free_move = true;
        CHECK(solver_likho(l, NULL));
        size_t after = solver_unsatisfied_count(l);
        if (free_move) { CHECK(after < before); with_free++; }
        else CHECK(after <= before);
        solver_free(m); solver_free(l); cnf_free(&c);
    }
    CHECK(with_free > 20);   /* the interesting branch really happens */
}

static void test_likho_general_cnf_and_edge_cases(void) {
    for (int i = 0; i < 120; i++) {                         /* units, long clauses, unused variables */
        int n = 6 + (i % 6) * 7;
        Cnf c = random_general_cnf(n * 2, n + (i % 5) * 6, 1 + (i % 6), 8100 + (uint64_t)i);
        Solver *base = must_solver(&c, 2), *a = must_solver(&c, 2), *b = must_solver(&c, 2);
        init_zero(base, c.n_vars);
        CHECK(solver_likho(a, NULL) && solver_likho(b, NULL));
        CHECK(c.n_vars == 0 || memcmp(solver_assignment(a), solver_assignment(b), c.n_vars) == 0);
        CHECK(solver_unsatisfied_count(a) <= solver_unsatisfied_count(base));
        CHECK(solver_unsatisfied_count(a) == brute_unsat(&c, solver_assignment(a)));
        solver_free(base); solver_free(a); solver_free(b);
        cnf_free(&c);
    }
    const char *edge[] = { "p cnf 0 0\n", "p cnf 5 0\n", "p cnf 1 1\n1 0\n", "p cnf 3 1\n1 2 0\n" };
    for (size_t i = 0; i < sizeof edge / sizeof edge[0]; i++) {
        Cnf c = must_parse(edge[i]);
        Solver *s = must_solver(&c, 1);
        CHECK(solver_likho(s, NULL));
        CHECK(solver_unsatisfied_count(s) == brute_unsat(&c, solver_assignment(s)));
        solver_free(s); cnf_free(&c);
    }
}

static void test_likho_in_group_mode(void) {
    SolverConfig cfg = solver_config_default();
    cfg.init = INIT_LIKHO;
    cfg.max_steps = 5000; cfg.restarts = 2;
    for (int i = 0; i < 40; i++) {
        int n = 40 + (i % 4) * 25;
        Cnf c = random_cnf(n, (int)(n * (2.2 + 0.3 * (i % 5))), 7100 + (uint64_t)i);
        GroupConfig gc = group_config_default();
        gc.max_groups = 64;
        ResultGroup a = group_solve(&c, &cfg, &gc), b = group_solve(&c, &cfg, &gc);
        CHECK(a.ok && b.ok);
        if (a.ok && b.ok) {
            check_group_result(&c, &a.val);              /* prime/neutral lists and counters are consistent */
            CHECK(same_group_result(&a.val, &b.val));
        }
        if (a.ok) group_result_free(&a.val);
        if (b.ok) group_result_free(&b.val);
        cnf_free(&c);
    }
}

/* ---- glowest + min-overlap random sweeps --------------------------------- */

static void test_glowest_random(void) {
    for (int i = 0; i < 60; i++) {
        int n = 40 + (i % 5) * 30, ratio_x10 = 20 + (i % 7) * 5;
        Cnf c = random_cnf(n, n * ratio_x10 / 10, 1100 + (uint64_t)i);
        Solver *a = must_solver(&c, 1), *b = must_solver(&c, 99);
        CHECK(solver_init_glowest(a) && solver_init_glowest(b));
        CHECK(c.n_vars == 0 || memcmp(solver_assignment(a), solver_assignment(b), c.n_vars) == 0);
        CHECK(solver_unsatisfied_count(a) == brute_unsat(&c, solver_assignment(a)));
        solver_free(a); solver_free(b); cnf_free(&c);
    }
    for (int i = 0; i < 40; i++) {
        Cnf c = random_general_cnf(30 + (i % 6) * 10, 40 + (i % 5) * 12, 1 + (i % 6), 1200 + (uint64_t)i);
        Solver *a = must_solver(&c, 1), *b = must_solver(&c, 99);
        CHECK(solver_init_glowest(a) && solver_init_glowest(b));
        CHECK(c.n_vars == 0 || memcmp(solver_assignment(a), solver_assignment(b), c.n_vars) == 0);
        CHECK(solver_unsatisfied_count(a) == brute_unsat(&c, solver_assignment(a)));
        solver_free(a); solver_free(b); cnf_free(&c);
    }
}

static void test_min_overlap_random(void) {
    for (int i = 0; i < 60; i++) {
        int n = 40 + (i % 5) * 30, ratio_x10 = 20 + (i % 7) * 5;
        Cnf c = random_cnf(n, n * ratio_x10 / 10, 1300 + (uint64_t)i);
        Solver *a = must_solver(&c, 1), *b = must_solver(&c, 99);
        CHECK(solver_init_min_overlap(a) && solver_init_min_overlap(b));
        CHECK(c.n_vars == 0 || memcmp(solver_assignment(a), solver_assignment(b), c.n_vars) == 0);
        CHECK(solver_unsatisfied_count(a) == brute_unsat(&c, solver_assignment(a)));
        solver_free(a); solver_free(b); cnf_free(&c);
    }
}

/* glowest and min-overlap must produce different assignments on some instance
 * (otherwise one is redundant). */
static void test_glowest_vs_min_overlap_differ(void) {
    int found = 0;
    for (int i = 0; i < 100 && !found; i++) {
        Cnf c = random_cnf(60, 180, 2000 + (uint64_t)i);
        Solver *g = must_solver(&c, 1), *m = must_solver(&c, 1);
        CHECK(solver_init_glowest(g) && solver_init_min_overlap(m));
        if (c.n_vars > 0 && memcmp(solver_assignment(g), solver_assignment(m), c.n_vars) != 0) found = 1;
        solver_free(g); solver_free(m); cnf_free(&c);
    }
    CHECK(found);
}

static size_t progress_calls = 0;
static void count_progress(void *ctx, size_t steps, size_t unsat) {
    (void)steps; (void)unsat; (*(size_t *)ctx)++;
}

/* Build a random 3-CNF in memory and solve it with every strategy. */
static void test_random_instances_and_strategies(void) {
    enum { N = 150, M = 570 };  /* ratio 3.8: comfortably satisfiable */
    static char text[M * 40 + 64];
    Rng g = rng_new(2024);
    int len = snprintf(text, sizeof text, "p cnf %d %d\n", N, M);
    for (int i = 0; i < M; i++) {
        int v[3];
        for (int k = 0; k < 3; k++) {
            int x, dup;
            do { x = 1 + (int)rng_index(&g, N); dup = 0; for (int j = 0; j < k; j++) dup |= (v[j] == x); } while (dup);
            v[k] = x;
        }
        for (int k = 0; k < 3; k++) len += snprintf(text + len, sizeof text - (size_t)len, "%d ", rng_bool(&g) ? v[k] : -v[k]);
        len += snprintf(text + len, sizeof text - (size_t)len, "0\n");
    }
    Cnf c = must_parse(text);
    CHECK(cnf_is_3sat(&c));
    for (size_t k = 0; k < N_STRATEGIES; k++) {
        CHECK(pick_fn_by_name(STRATEGIES[k].name) == STRATEGIES[k].fn);
        Solver *s = must_solver(&c, 77);
        SolverConfig cfg = solver_config_default();
        cfg.pick = STRATEGIES[k].fn;   /* function stored in a variable */
        cfg.max_steps = 200000; cfg.restarts = 10;
        cfg.on_progress = count_progress; cfg.progress_ctx = &progress_calls; cfg.progress_every = 1000;
        ResultSolve r = solver_solve(s, &cfg);
        if (strcmp(STRATEGIES[k].name, "ranked") == 0) {
            CHECK(r.ok && r.val.status == SOLVE_SAT && verify(&c, r.val.assignment, r.val.n_vars));
        } else if (r.ok && r.val.status == SOLVE_SAT) {
            CHECK(verify(&c, r.val.assignment, r.val.n_vars));   /* if it claims SAT it must be right */
        }
        if (r.ok) solve_result_free(&r.val);
        solver_free(s);
    }
    CHECK(pick_fn_by_name("nope") == NULL);
    CHECK(progress_calls > 0);
    cnf_free(&c);
}

/* TRY / TRY_CTX behave like Rust's `?`. */
typedef RESULT(int) ResultInt;
static ResultUnit inner_fails(void) { return ERRF(ResultUnit, ERR_PARSE, "boom"); }
static ResultInt outer(void) {
    (void)TRY_CTX(ResultInt, inner_fails(), "while testing");
    return OK(ResultInt, 1);
}
static void test_try_macros(void) {
    ResultInt r = outer();
    CHECK(!r.ok && r.err.kind == ERR_PARSE && strcmp(r.err.msg, "while testing: boom") == 0);
}

int main(void) {
    test_dimacs_multiline_and_comments();
    test_dimacs_percent_eof();
    test_normalization();
    test_no_twins();
    test_parse_errors();
    test_verify();
    test_solves_simple();
    test_empty_clause_and_empty_formula();
    test_unsat_is_unknown_never_unsat();
    test_contradictory_units_are_unsat();
    test_contradiction_lives_in_extras();
    test_negated_3clauses_are_not_unsat();
    test_rogue_is_a_clause_state();
    test_neutral_can_occur_in_rogue_clause();
    test_prime_precedence();
    test_flip_updates_state();
    test_incremental_matches_rebuild();
    test_random_instances_and_strategies();
    test_try_macros();
    test_ranking_invariant_neutral_means_zero_breaks();
    test_groups_small_and_edge_cases();
    test_groups_contracts_on_random_instances();
    test_group_len_limits_group_size();
    test_more_than_64_groups();
    test_init_rarest_hand_checked();
    test_init_glowest_hand_checked();
    test_init_yanus();
    test_init_min_overlap_hand_checked();
    test_init_gob_hand_checked();
    test_init_gob_sign_hand_checked();
    test_init_gob_square_hand_checked();
    test_init_rarest_properties();
    test_init_gob_properties();
    test_init_gob_sign_properties();
    test_init_gob_square_properties();
    test_glowest_random();
    test_min_overlap_random();
    test_glowest_vs_min_overlap_differ();
    test_likho_neutral_repair();
    test_likho_domain_repair();
    test_likho_switch_domain_chain();
    test_likho_deep_dive_domains_first();
    test_likho_pre_cook();
    test_likho_properties();
    test_init_rarest_matches_reference();
    test_init_glowest_matches_reference();
    test_init_min_overlap_matches_reference();
    test_likho_refreshed_states();
    test_likho_strictly_improves_when_a_free_move_exists();
    test_likho_general_cnf_and_edge_cases();
    test_likho_in_group_mode();
    test_terminator_hand_checked();
    test_terminator_properties();
    test_glowest_likho_terminator_solve();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
