#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
