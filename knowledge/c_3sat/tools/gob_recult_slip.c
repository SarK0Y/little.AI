/* Dynamics of gob-recult sign decoding vs a known model ("sign slipping").
 *
 * Replays solver_init_gob_recult's Kaczmarz loop and, each sampled sweep,
 * reports how the sign-decode of x (x_i > 0) agrees with a reference model,
 * how many signs flipped since the previous sample, and unsatisfied clauses
 * of that decode on the original CNF.
 *
 * usage:
 *   gob_recult_slip <file.cnf> --model "-1 2 3 0" [opts]
 *   gob_recult_slip <file.cnf> --model-file /tmp/opencode/aim17_models.txt [opts]
 * opts: --seed N  -play-baseline P  -set-baseline N  --every K (default 1)
 *       --no-repair   (skip final gob_repair in report)
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sat.h"

#define GOB_SWEEPS 64
#define GOB_PASSES 4

static bool parse_model_str(const char *s, bool *a, size_t nv) {
    for (size_t i = 0; i < nv; i++) a[i] = false;
    char *dup = strdup(s);
    if (!dup) return false;
    for (char *t = strtok(dup, " \t\r\n"); t; t = strtok(NULL, " \t\r\n")) {
        long v = strtol(t, NULL, 10);
        if (v == 0) break;
        size_t idx = (v < 0 ? (size_t)(-v) : (size_t)v);
        if (idx == 0 || idx > nv) { free(dup); return false; }
        a[idx - 1] = v > 0;
    }
    free(dup);
    return true;
}

static bool load_model_for(const char *cnf_path, const char *mfile, bool *a, size_t nv) {
    FILE *f = fopen(mfile, "r");
    if (!f) { perror(mfile); return false; }
    char line[4096];
    int want = 0;
    const char *base = strrchr(cnf_path, '/');
    base = base ? base + 1 : cnf_path;
    bool ok = false;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "FILE ", 5) == 0) {
            char *p = line + 5;
            while (*p == ' ' || *p == '\t') p++;
            char *end = p + strlen(p);
            while (end > p && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = '\0';
            const char *b = strrchr(p, '/');
            b = b ? b + 1 : p;
            want = (strcmp(b, base) == 0);
        } else if (want && strncmp(line, "MODEL ", 6) == 0) {
            ok = parse_model_str(line + 6, a, nv);
            break;
        }
    }
    fclose(f);
    if (!ok) fprintf(stderr, "model for %s not found/parsed in %s\n", base, mfile);
    return ok;
}

static size_t count_unsat_decode(const Cnf *cnf, const bool *a) {
    size_t bad = 0;
    for (size_t ci = 0; ci < cnf->n_clauses; ci++) {
        const Lit *cl = cnf_clause(cnf, ci);
        size_t len = cnf_clause_len(cnf, ci);
        bool sat = false;
        for (size_t k = 0; k < len; k++) {
            if ((cl[k].positive && a[cl[k].var]) || (!cl[k].positive && !a[cl[k].var])) { sat = true; break; }
        }
        if (!sat) bad++;
    }
    return bad;
}

static void decode_sign(const double *x, bool *d, size_t nv) {
    for (size_t i = 0; i < nv; i++) d[i] = x[i] > 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: gob_recult_slip <file.cnf> --model \"...\"|--model-file F [--seed N] [-play-baseline P] [-set-baseline N] [--every K]\n");
        return 2;
    }
    const char *path = argv[1];
    const char *model_str = NULL, *model_file = NULL;
    uint64_t seed = 0x9e3779b97f4a7c15ULL;
    size_t play = 0, every = 1;
    double setb = 0.0; bool has_set = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) model_str = argv[++i];
        else if (strcmp(argv[i], "--model-file") == 0 && i + 1 < argc) model_file = argv[++i];
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
        else if ((strcmp(argv[i], "-play-baseline") == 0 || strcmp(argv[i], "--play-baseline") == 0) && i + 1 < argc) play = strtoull(argv[++i], NULL, 10);
        else if ((strcmp(argv[i], "-set-baseline") == 0 || strcmp(argv[i], "--set-baseline") == 0) && i + 1 < argc) { setb = strtod(argv[++i], NULL); has_set = true; }
        else if (strcmp(argv[i], "--every") == 0 && i + 1 < argc) every = strtoull(argv[++i], NULL, 10);
        else { fprintf(stderr, "unknown/invalid arg: %s\n", argv[i]); return 2; }
    }
    if (every == 0) every = 1;

    ResultCnf rc = cnf_from_dimacs_file(path);
    if (!rc.ok) { fprintf(stderr, "parse: %s\n", rc.err.msg); return 1; }
    Cnf *cnf = &rc.val;
    size_t nv = cnf->n_vars;
    bool *model = calloc(nv ? nv : 1, 1);
    if (!model) { fprintf(stderr, "oom\n"); return 1; }
    if (model_str) {
        if (!parse_model_str(model_str, model, nv)) { fprintf(stderr, "bad --model\n"); return 1; }
    } else if (model_file) {
        if (!load_model_for(path, model_file, model, nv)) return 1;
    } else {
        fprintf(stderr, "need --model or --model-file\n"); return 1;
    }

    Cnf filtered = {0};
    if (!no_twins(cnf, &filtered)) { fprintf(stderr, "no_twins oom\n"); return 1; }
    size_t nc = filtered.n_clauses;
    size_t nx = nv ? nv : 1, ny = nc ? nc : 1;
    double *x = calloc(nx, sizeof *x);
    double *y = calloc(ny, sizeof *y);
    bool *dec = calloc(nx, 1), *prev = calloc(nx, 1);
    if (!x || !y || !dec || !prev) { fprintf(stderr, "oom\n"); return 1; }

    Rng rng = rng_new(seed);

    double baseline = 0.0;
    if (has_set) baseline = setb;
    else if (play > 0) baseline = (double)(rng_next_u64(&rng) % play);

    size_t k = 2;
    if (nc > 0) {
        k = (size_t)(rng_next_u64(&rng) % nc);
        if (k == 0) k = 1 % nc;
        if (k == 0) k = 1;
        if (k % 2 == 1) k = (k + 1 <= nc ? k + 1 : k - 1);
        if (k == 0) k = 2;
    }
    size_t h = k / 2;

    printf("# file=%s nv=%zu nc=%zu(nc_f=%zu) seed=%llu baseline=%g k=%zu h=%zu\n",
           path, nv, cnf->n_clauses, nc, (unsigned long long)seed, baseline, k, h);
    printf("# sweep\tagree\tdisagree\tagree_pct\tflips_prev\tunsat_dec\n");

    /* initial state (sweep 0, before projections) */
    decode_sign(x, dec, nv);
    memcpy(prev, dec, nv);
    size_t agree0 = 0, flip0 = 0;
    for (size_t i = 0; i < nv; i++) if (dec[i] == model[i]) agree0++;
    printf("0\t%zu\t%zu\t%.2f\t%zu\t%zu\n", agree0, nv - agree0, nv ? 100.0*agree0/nv : 0.0, flip0, count_unsat_decode(cnf, dec));

    for (size_t sweep = 1; sweep <= GOB_SWEEPS; sweep++) {
        for (size_t ci = 0; ci < nc; ci++) {
            const Lit *cl = cnf_clause(&filtered, ci);
            size_t len = cnf_clause_len(&filtered, ci);
            size_t j1 = nc ? (ci + k) % nc : 0;
            size_t j2 = nc ? (ci + h) % nc : 0;
            double s1 = ((ci + k) % 2 == 0) ? 1.0 : -1.0;
            double sum = 0;
            for (size_t t = 0; t < len; t++)
                sum += cl[t].positive ? x[cl[t].var] : -x[cl[t].var];
            double rhs = 1.0 + y[ci] + s1 * y[j1] + y[j2];
            double dot = sum - rhs;
            double anorm = (double)len + 3.0;
            double step = dot / anorm;
            if (step == 0.0) continue;
            for (size_t t = 0; t < len; t++)
                x[cl[t].var] -= step * (cl[t].positive ? 1.0 : -1.0);
            y[ci] -= step * (-1.0);
            y[j1] -= step * (-s1);
            y[j2] -= step * (-1.0);
        }
        if (nc > 0) { /* coupling: sum Y_i = baseline */
            double sumy = 0;
            for (size_t i = 0; i < nc; i++) sumy += y[i];
            double step = (sumy - baseline) / (double)nc;
            if (step != 0.0)
                for (size_t i = 0; i < nc; i++) y[i] -= step;
        }
        if (sweep % every == 0 || sweep == GOB_SWEEPS) {
            decode_sign(x, dec, nv);
            size_t agree = 0, flips = 0;
            for (size_t i = 0; i < nv; i++) {
                if (dec[i] == model[i]) agree++;
                if (dec[i] != prev[i]) flips++;
            }
            printf("%zu\t%zu\t%zu\t%.2f\t%zu\t%zu\n", sweep, agree, nv - agree,
                   nv ? 100.0*agree/nv : 0.0, flips, count_unsat_decode(cnf, dec));
            memcpy(prev, dec, nv);
        }
    }

    /* final with repair, via solver (to reuse gob_repair exactly) */
    SolverConfig cfg = solver_config_default();
    cfg.seed = seed;
    cfg.gob_recult_play = play;
    cfg.gob_recult_set = setb;
    cfg.gob_recult_has_set = has_set;
    ResultSolver rs = solver_new(cnf, seed);
    if (rs.ok) {
        if (solver_init_gob_recult(rs.val, &cfg)) {
            const bool *a = solver_assignment(rs.val);
            size_t agree = 0;
            for (size_t i = 0; i < nv; i++) if (a[i] == model[i]) agree++;
            printf("# after_repair\tagree=%zu/%zu(%.2f%%)\tunsat=%zu\n",
                   agree, nv, nv ? 100.0*agree/nv : 0.0, solver_unsatisfied_count(rs.val));
        }
        solver_free(rs.val);
    }

    free(x); free(y); free(dec); free(prev); free(model);
    cnf_free(&filtered);
    cnf_free(cnf);
    return 0;
}
