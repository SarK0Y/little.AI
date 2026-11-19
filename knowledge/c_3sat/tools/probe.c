/* Diagnostic harness for the gob/gob-sign projection.
 *
 * Mirrors bench_rarest.c's random generator and parser byte-for-byte, dumps
 * each parsed CNF, and runs only the gob-sign start:
 *
 *   probe gen <n> <ratio> <seeds> <steps> <restarts> [outdir]
 *       writes <outdir>/b<seed>.cnf, prints init_unsat and SAT/UNKNOWN per seed
 *   probe run <file.cnf> <steps> <restarts>
 *   probe start <file.cnf> <out.txt>
 *       writes the repaired gob-sign bits, one char per variable
 *   probe xstart <startfile.cnf> <solvefile.cnf> <steps> <restarts>
 *       take the gob-sign start from startfile, solve solvefile (same vars)
 *
 * The last two modes separate the decoded start from the search's clause order.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "sat.h"

typedef struct { uint64_t s; } R;
static uint64_t nx(R *r) { uint64_t x = r->s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return r->s = x; }
static size_t idx(R *r, size_t n) { return (size_t)(nx(r) % n); }

typedef struct { size_t nv, nc, *off; int *lits; } Gen;
static void gen_free(Gen *g) { free(g->off); free(g->lits); }

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

static void dump(const Cnf *cnf, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "p cnf %zu %zu\n", cnf->n_vars, cnf->n_clauses);
    for (size_t c = 0; c < cnf->n_clauses; c++) {
        size_t len = cnf_clause_len(cnf, c);
        const Lit *cl = cnf_clause(cnf, c);
        for (size_t k = 0; k < len; k++)
            fprintf(f, "%d ", cl[k].positive ? (int)cl[k].var + 1 : -(int)cl[k].var - 1);
        fprintf(f, "0\n");
    }
    fclose(f);
}

/* gob-sign init, report init quality, then solve. */
static void run(const Cnf *cnf, size_t steps, size_t restarts, const char *tag) {
    SolverConfig cfg = solver_config_default();
    cfg.max_steps = steps;
    cfg.restarts = restarts;
    ResultSolver rs = solver_new(cnf, cfg.seed);
    if (!rs.ok) { fprintf(stderr, "solver: %s\n", rs.err.msg); exit(1); }
    Solver *s = rs.val;
    if (!solver_init_gob_sign(s)) { fprintf(stderr, "init: oom\n"); exit(1); }
    size_t init_unsat = solver_unsatisfied_count(s);
    if (steps == 0) {
        printf("%s\tinit_unsat=%zu\t(no search)\n", tag, init_unsat);
        solver_free(s);
        return;
    }
    bool *start = malloc(cnf->n_vars * sizeof *start);
    memcpy(start, solver_assignment(s), cnf->n_vars * sizeof *start);
    SolverConfig c = cfg;
    c.initial = start;
    ResultSolve r = solver_solve(s, &c);
    if (!r.ok) { fprintf(stderr, "solve: %s\n", r.err.msg); exit(1); }
    printf("%s\tinit_unsat=%zu\t%s\tsteps=%zu\n", tag, init_unsat,
           r.val.status == SOLVE_SAT ? "SAT" : "UNKNOWN", r.val.steps);
    solve_result_free(&r.val);
    free(start);
    solver_free(s);
}

/* Start from the gob-sign decode of startfile, but solve solvefile (same vars). */
static void cross(const Cnf *startfile, const Cnf *solvefile, size_t steps,
                  size_t restarts, const char *tag) {
    if (startfile->n_vars != solvefile->n_vars) {
        fprintf(stderr, "xstart: variable count differs\n");
        exit(1);
    }
    SolverConfig cfg = solver_config_default();
    cfg.max_steps = steps;
    cfg.restarts = restarts;
    ResultSolver rs = solver_new(startfile, cfg.seed);
    if (!rs.ok) { fprintf(stderr, "solver: %s\n", rs.err.msg); exit(1); }
    if (!solver_init_gob_sign(rs.val)) { fprintf(stderr, "init: oom\n"); exit(1); }
    bool *start = malloc(solvefile->n_vars * sizeof *start);
    memcpy(start, solver_assignment(rs.val), solvefile->n_vars * sizeof *start);
    solver_free(rs.val);
    ResultSolver rt = solver_new(solvefile, cfg.seed);
    if (!rt.ok) { fprintf(stderr, "solver: %s\n", rt.err.msg); exit(1); }
    SolverConfig c = cfg;
    c.initial = start;
    ResultSolve r = solver_solve(rt.val, &c);
    if (!r.ok) { fprintf(stderr, "solve: %s\n", r.err.msg); exit(1); }
    printf("%s\t%s\tsteps=%zu\n", tag,
           r.val.status == SOLVE_SAT ? "SAT" : "UNKNOWN", r.val.steps);
    solve_result_free(&r.val);
    solver_free(rt.val);
    free(start);
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "start") == 0 && argc >= 4) {
        ResultCnf rc = cnf_from_dimacs_file(argv[2]);
        if (!rc.ok) { fprintf(stderr, "parse: %s\n", rc.err.msg); return 1; }
        SolverConfig cfg = solver_config_default();
        ResultSolver rs = solver_new(&rc.val, cfg.seed);
        if (!rs.ok || !solver_init_gob_sign(rs.val)) { fprintf(stderr, "init failed\n"); return 1; }
        const bool *a = solver_assignment(rs.val);
        FILE *f = fopen(argv[3], "w");
        if (!f) { perror(argv[3]); return 1; }
        for (size_t i = 0; i < rc.val.n_vars; i++) fputc(a[i] ? '1' : '0', f);
        fputc('\n', f);
        fclose(f);
        solver_free(rs.val);
        cnf_free(&rc.val);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "xstart") == 0 && argc >= 6) {
        ResultCnf a = cnf_from_dimacs_file(argv[2]);
        ResultCnf b = cnf_from_dimacs_file(argv[3]);
        if (!a.ok || !b.ok) { fprintf(stderr, "parse failed\n"); return 1; }
        cross(&a.val, &b.val, strtoull(argv[4], NULL, 10), strtoull(argv[5], NULL, 10), argv[2]);
        cnf_free(&a.val); cnf_free(&b.val);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "run") == 0 && argc >= 5) {
        ResultCnf rc = cnf_from_dimacs_file(argv[2]);
        if (!rc.ok) { fprintf(stderr, "parse: %s\n", rc.err.msg); return 1; }
        run(&rc.val, strtoull(argv[3], NULL, 10), strtoull(argv[4], NULL, 10), argv[2]);
        cnf_free(&rc.val);
        return 0;
    }
    if (argc >= 7 && strcmp(argv[1], "gen") == 0) {
        size_t nv = strtoull(argv[2], NULL, 10);
        double ratio = atof(argv[3]);
        size_t seeds = strtoull(argv[4], NULL, 10);
        size_t steps = strtoull(argv[5], NULL, 10);
        size_t restarts = strtoull(argv[6], NULL, 10);
        const char *outdir = argc >= 8 ? argv[7] : ".";
        size_t nc = (size_t)((double)nv * ratio + 0.5);
        for (size_t sd = 1; sd <= seeds; sd++) {
            Gen g = gen3(nv, nc, 0x9e3779b97f4a7c15ULL ^ (sd * 0x100000001b3ULL));
            char *d = to_dimacs(&g);
            ResultCnf rc = cnf_from_dimacs_str(d);
            free(d);
            if (!rc.ok) { fprintf(stderr, "parse: %s\n", rc.err.msg); return 1; }
            nc = rc.val.n_clauses;                  /* bench feeds parsed count forward */
            char path[512], tag[64];
            snprintf(path, sizeof path, "%s/b%zu.cnf", outdir, sd);
            snprintf(tag, sizeof tag, "seed=%zu clauses=%zu", sd, nc);
            dump(&rc.val, path);
            run(&rc.val, steps, restarts, tag);
            cnf_free(&rc.val);
            gen_free(&g);
        }
        return 0;
    }
    fprintf(stderr,
        "usage: probe gen <n> <ratio> <seeds> <steps> <restarts> [outdir]\n"
        "       probe run <file.cnf> <steps> <restarts>\n"
        "       probe start <file.cnf> <out.txt>\n"
        "       probe xstart <startfile.cnf> <solvefile.cnf> <steps> <restarts>\n");
    return 2;
}
