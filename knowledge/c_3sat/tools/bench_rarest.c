/* Benchmark of the constructive `--init rarest` start against the default
 * majority start, on random 3-SAT or a DIMACS file.
 *
 * The previous total-occurrence rarest order is reimplemented here as a
 * baseline (O(nv * clauses), benchmark-sized inputs only) and injected through
 * SolverConfig.initial, so all three starts run the same search code.
 *
 * usage:
 *   bench_rarest <n> <ratio> <seeds> <steps> <restarts>
 *   bench_rarest <file.cnf> <steps> <restarts>
 *
 * steps = 0 measures only the init quality (no search).
 * Every SAT result is verified against the formula before it is counted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include "sat.h"

/* ---- random 3-SAT ------------------------------------------------------ */

typedef struct { uint64_t s; } R;
static uint64_t nx(R *r) { uint64_t x = r->s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return r->s = x; }
static size_t idx(R *r, size_t n) { return (size_t)(nx(r) % n); }

typedef struct { size_t nv, nc, *off; int *lits; } Gen;

static void gen_free(Gen *g) { free(g->off); free(g->lits); }

/* Three distinct variables per clause, random polarity. */
static Gen gen3(size_t nv, size_t nc, uint64_t seed) {
    Gen g = { nv, nc, malloc((nc + 1) * sizeof *g.off), malloc(nc * 3 * sizeof *g.lits) };
    R r = { seed ? seed : 1 };
    g.off[0] = 0;
    for (size_t c = 0; c < nc; c++) {
        size_t a = idx(&r, nv), b = idx(&r, nv - 1), d = idx(&r, nv - 2);
        if (b >= a) b++;
        if (d >= a) d++;
        if (d >= b) d++;
        int s[3];
        for (int k = 0; k < 3; k++) s[k] = (nx(&r) & 1) ? 1 : -1;
        g.lits[3*c+0] = s[0] * (int)(a + 1);
        g.lits[3*c+1] = s[1] * (int)(b + 1);
        g.lits[3*c+2] = s[2] * (int)(d + 1);
        g.off[c+1] = g.off[c] + 3;
    }
    return g;
}

static char *to_dimacs(const Gen *g) {
    size_t cap = g->nc * 32 + 64;
    char *s = malloc(cap), *p = s;
    p += sprintf(p, "p cnf %zu %zu\n", g->nv, g->nc);
    for (size_t c = 0; c < g->nc; c++) {
        for (int k = 0; k < 3; k++) p += sprintf(p, "%d ", g->lits[3*c+k]);
        *p++ = '0'; *p++ = '\n'; *p = '\0';
    }
    return s;
}

/* ---- old rarest: static order by total occurrences, ties by id --------- */

static void old_rarest(const Cnf *cnf, bool *a) {
    size_t nv = cnf->n_vars, nc = cnf->n_clauses;
    size_t *cnt = calloc(nv, sizeof *cnt), *order = malloc(nv * sizeof *order);
    for (size_t c = 0; c < nc; c++)
        for (size_t k = 0; k < cnf_clause_len(cnf, c); k++) cnt[cnf_clause(cnf, c)[k].var]++;
    for (size_t v = 0; v < nv; v++) order[v] = v;
    for (size_t i = 0; i < nv; i++)
        for (size_t j = i + 1; j < nv; j++)
            if (cnt[order[j]] < cnt[order[i]]) { size_t t = order[i]; order[i] = order[j]; order[j] = t; }
    uint32_t *unset = malloc(nc * sizeof *unset);
    unsigned char *done = calloc(nc, 1);
    for (size_t c = 0; c < nc; c++) unset[c] = (uint32_t)cnf_clause_len(cnf, c);
    for (size_t i = 0; i < nv; i++) {
        size_t v = order[i], fp = 0, fn = 0, op = 0, on = 0, tp = 0, tn = 0;
        for (size_t c = 0; c < nc; c++)
            for (size_t k = 0; k < cnf_clause_len(cnf, c); k++) {
                const Lit l = cnf_clause(cnf, c)[k];
                if (l.var != v) continue;
                if (l.positive) tp++; else tn++;
                if (done[c]) continue;
                if (unset[c] == 1) { if (l.positive) fp++; else fn++; }
                else { if (l.positive) op++; else on++; }
            }
        bool val = (fp != fn) ? fp > fn : (op != on) ? op > on : (tp != tn) ? tp > tn : false;
        a[v] = val;
        for (size_t c = 0; c < nc; c++)
            for (size_t k = 0; k < cnf_clause_len(cnf, c); k++) {
                const Lit l = cnf_clause(cnf, c)[k];
                if (l.var != v) continue;
                unset[c]--;
                if (!done[c] && l.positive == val) done[c] = 1;
            }
    }
    free(cnt); free(order); free(unset); free(done);
}

/* ---- harness ----------------------------------------------------------- */

typedef struct {
    const char *name;
    size_t init_unsat, solved, sum_steps;
    double sec;
} Row;

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <n> <ratio> <seeds> <steps> <restarts>\n"
                        "       %s <file.cnf> <steps> <restarts>\n", argv[0], argv[0]);
        return 2;
    }
    char *end;
    strtoull(argv[1], &end, 10);
    bool file_mode = !(argv[1][0] && *end == '\0');
    size_t nv = 0, nc = 0, seeds = 1, steps, restarts;
    double ratio = 0.0;
    if (file_mode) {
        if (argc < 4) {
            fprintf(stderr, "usage: %s <file.cnf> <steps> <restarts>\n", argv[0]);
            return 2;
        }
        steps = strtoull(argv[2], NULL, 10);
        restarts = strtoull(argv[3], NULL, 10);
    } else {
        if (argc < 6) {
            fprintf(stderr, "usage: %s <n> <ratio> <seeds> <steps> <restarts>\n", argv[0]);
            return 2;
        }
        nv = strtoull(argv[1], NULL, 10);
        ratio = atof(argv[2]);
        seeds = strtoull(argv[3], NULL, 10);
        steps = strtoull(argv[4], NULL, 10);
        restarts = strtoull(argv[5], NULL, 10);
        nc = (size_t)(nv * ratio + 0.5);
    }

    Row rows[3] = {
        { .name = "majority" }, { .name = "rarest-old" }, { .name = "rarest-new" },
    };
    for (size_t sd = 1; sd <= seeds; sd++) {
        Gen g = { 0 };
        char *d = NULL;
        ResultCnf rc;
        if (file_mode) {
            rc = cnf_from_dimacs_file(argv[1]);
        } else {
            g = gen3(nv, nc, 0x9e3779b97f4a7c15ULL ^ (sd * 0x100000001b3ULL));
            d = to_dimacs(&g);
            rc = cnf_from_dimacs_str(d);
            free(d);
        }
        if (!rc.ok) { fprintf(stderr, "parse: %s\n", rc.err.msg); return 1; }
        Cnf cnf = rc.val;
        nv = cnf.n_vars; nc = cnf.n_clauses;

        for (int i = 0; i < 3; i++) {
            ResultSolver rs = solver_new(&cnf, 5);
            if (!rs.ok) { fprintf(stderr, "solver: %s\n", rs.err.msg); return 1; }
            Solver *s = rs.val;
            if (i == 0) {
                solver_init_majority(s);
            } else if (i == 1) {
                bool *a = calloc(nv, sizeof *a);
                old_rarest(&cnf, a);
                solver_set_assignment(s, a);
                free(a);
            } else if (!solver_init_rarest(s)) {
                fprintf(stderr, "init: out of memory\n");
                return 1;
            }

            rows[i].init_unsat += solver_unsatisfied_count(s);
            if (steps) {
                bool *start = malloc(nv * sizeof *start);
                memcpy(start, solver_assignment(s), nv * sizeof *start);
                SolverConfig c = solver_config_default();
                c.max_steps = steps;
                c.restarts = restarts;
                c.initial = start;
                clock_t t0 = clock();
                ResultSolve r = solver_solve(s, &c);
                rows[i].sec += (double)(clock() - t0) / CLOCKS_PER_SEC;
                if (!r.ok) { fprintf(stderr, "solve: %s\n", r.err.msg); return 1; }
                if (r.val.status == SOLVE_SAT) {
                    if (!verify(&cnf, r.val.assignment, r.val.n_vars)) {
                        fprintf(stderr, "BAD ASSIGNMENT\n");
                        return 1;
                    }
                    rows[i].solved++;
                    rows[i].sum_steps += r.val.steps;
                }
                solve_result_free(&r.val);
                free(start);
            }
            solver_free(s);
        }
        cnf_free(&cnf);
        gen_free(&g);
        fputc('.', stderr);
    }
    fputc('\n', stderr);

    if (file_mode) printf("%s  vars=%zu clauses=%zu\n", argv[1], nv, nc);
    else printf("n=%zu ratio=%.1f clauses=%zu seeds=%zu\n", nv, ratio, nc, seeds);
    printf("budget=%zu steps x %zu attempts\n", steps, restarts + 1);
    printf("%-12s %12s %8s %12s %8s\n", "init", "init_unsat", "solved", "mean_steps", "sec");
    for (int i = 0; i < 3; i++) {
        double init = (double)rows[i].init_unsat / (double)seeds;
        double mean = rows[i].solved ? (double)rows[i].sum_steps / (double)rows[i].solved : 0.0;
        printf("%-12s %12.1f %5zu/%-2zu %12.0f %8.1f\n",
               rows[i].name, init, rows[i].solved, seeds, mean, rows[i].sec);
    }
    return 0;
}
