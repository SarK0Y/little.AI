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
    test_init_rarest_properties();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
