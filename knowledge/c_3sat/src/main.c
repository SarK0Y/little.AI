#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "groups.h"
#include "sat.h"

#define DEFAULT_OUT "./knowledge/c_3sat/results/latest.json"

typedef struct {
    const char *input;
    const char *output;
    SolverConfig cfg;
    bool groups_mode;      /* --mode groups: sequential rogue groups + neutral merge */
    GroupConfig gcfg;
} Cli;

typedef RESULT(int) ResultInt;

/* ---- option handlers: each flag maps to a function stored in a table ---- */

typedef ResultUnit (*OptApply)(Cli *cli, const char *flag, const char *value);

static ResultUnit parse_size(const char *flag, const char *value, size_t *out) {
    char *end = NULL;
    errno = 0;
    if (!*value || *value == '-' || isspace((unsigned char)*value))
        return ERRF(ResultUnit, ERR_USAGE, "Invalid value for %s: `%s`", flag, value);
    unsigned long long v = strtoull(value, &end, 10);
    if (errno || *end || v > SIZE_MAX)
        return ERRF(ResultUnit, ERR_USAGE, "Invalid value for %s: `%s`", flag, value);
    *out = (size_t)v;
    return OK_UNIT(ResultUnit);
}

static ResultUnit opt_steps(Cli *cli, const char *flag, const char *value) {
    return parse_size(flag, value, &cli->cfg.max_steps);
}
static ResultUnit opt_restarts(Cli *cli, const char *flag, const char *value) {
    return parse_size(flag, value, &cli->cfg.restarts);
}
static ResultUnit opt_seed(Cli *cli, const char *flag, const char *value) {
    size_t v;
    TRY0(ResultUnit, parse_size(flag, value, &v));
    cli->cfg.seed = (uint64_t)v;
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_noise(Cli *cli, const char *flag, const char *value) {
    char *end = NULL;
    errno = 0;
    double v = strtod(value, &end);
    if (!*value || errno || *end) return ERRF(ResultUnit, ERR_USAGE, "Invalid value for %s: `%s`", flag, value);
    if (!(v >= 0.0 && v <= 1.0)) return ERRF(ResultUnit, ERR_USAGE, "--noise must be between 0 and 1");
    cli->cfg.noise = v;
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_out(Cli *cli, const char *flag, const char *value) {
    (void)flag;
    cli->output = value;
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_strategy(Cli *cli, const char *flag, const char *value) {
    PickFn fn = pick_fn_by_name(value);
    if (!fn) {
        char names[96] = "";
        for (size_t i = 0; i < N_STRATEGIES; i++) {
            if (i) strncat(names, "|", sizeof names - strlen(names) - 1);
            strncat(names, STRATEGIES[i].name, sizeof names - strlen(names) - 1);
        }
        return ERRF(ResultUnit, ERR_USAGE, "Invalid value for %s: `%s` (expected %s)", flag, value, names);
    }
    cli->cfg.pick = fn;
    return OK_UNIT(ResultUnit);
}

static ResultUnit opt_mode(Cli *cli, const char *flag, const char *value) {
    if (strcmp(value, "plain") == 0) cli->groups_mode = false;
    else if (strcmp(value, "groups") == 0) cli->groups_mode = true;
    else return ERRF(ResultUnit, ERR_USAGE, "Invalid value for %s: `%s` (expected plain|groups)", flag, value);
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_max_groups(Cli *cli, const char *flag, const char *value) {
    size_t v;
    TRY0(ResultUnit, parse_size(flag, value, &v));
    if (v < 1 || v > GROUPS_MAX) return ERRF(ResultUnit, ERR_USAGE, "%s must be between 1 and %d", flag, GROUPS_MAX);
    cli->gcfg.max_groups = v;
    return OK_UNIT(ResultUnit);
}

static ResultUnit opt_merge(Cli *cli, const char *flag, const char *value) {
    if (strcmp(value, "free") == 0) cli->gcfg.directed_only = false;
    else if (strcmp(value, "directed") == 0) cli->gcfg.directed_only = true;
    else return ERRF(ResultUnit, ERR_USAGE, "Invalid value for %s: `%s` (expected free|directed)", flag, value);
    return OK_UNIT(ResultUnit);
}

static ResultUnit opt_init(Cli *cli, const char *flag, const char *value) {
    if (strcmp(value, "majority") == 0) cli->cfg.init = INIT_MAJORITY;
    else if (strcmp(value, "rarest") == 0) cli->cfg.init = INIT_RAREST;
    else return ERRF(ResultUnit, ERR_USAGE, "Invalid value for %s: `%s` (expected majority|rarest)", flag, value);
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_group_len(Cli *cli, const char *flag, const char *value) {
    return parse_size(flag, value, &cli->gcfg.group_len);
}
static ResultUnit opt_diversify(Cli *cli, const char *flag, const char *value) {
    return parse_size(flag, value, &cli->gcfg.diversify);
}

static void print_progress(void *ctx, size_t steps, size_t unsat) {
    (void)ctx;
    fprintf(stderr, "[%zu steps] %zu unsatisfied\n", steps, unsat);
}
static ResultUnit opt_progress(Cli *cli, const char *flag, const char *value) {
    TRY0(ResultUnit, parse_size(flag, value, &cli->cfg.progress_every));
    cli->cfg.on_progress = cli->cfg.progress_every ? print_progress : NULL;
    return OK_UNIT(ResultUnit);
}

typedef struct { const char *flag; OptApply apply; } OptSpec;
static const OptSpec OPTS[] = {
    { "--steps", opt_steps },     { "--restarts", opt_restarts },
    { "--noise", opt_noise },     { "--seed", opt_seed },
    { "--out", opt_out },         { "--strategy", opt_strategy },
    { "--progress", opt_progress },
    { "--mode", opt_mode },       { "--max-groups", opt_max_groups },
    { "--merge", opt_merge },     { "--diversify", opt_diversify },
    { "--group-len", opt_group_len }, { "-group-len", opt_group_len },
    { "--init", opt_init },
};

static void usage(void) {
    fprintf(stderr,
        "Usage: 3sat INPUT.cnf [--steps N] [--restarts N] [--noise 0..1] [--seed N]\n"
        "                      [--strategy ranked|greedy|random] [--progress N] [--out FILE]\n"
        "                      [--mode plain|groups] [--max-groups 1..1024] [--merge free|directed]\n"
        "                      [--diversify N] [--group-len N]\n"
        "                      [--init majority|rarest]\n");
}

static ResultUnit parse_args(int argc, char **argv, Cli *cli) {
    if (argc < 2) return ERRF(ResultUnit, ERR_USAGE, "Missing INPUT.cnf");
    cli->input = argv[1];
    for (int i = 2; i < argc; i += 2) {
        const OptSpec *spec = NULL;
        for (size_t k = 0; k < sizeof OPTS / sizeof OPTS[0]; k++)
            if (strcmp(OPTS[k].flag, argv[i]) == 0) { spec = &OPTS[k]; break; }
        if (!spec) return ERRF(ResultUnit, ERR_USAGE, "Unknown option: %s", argv[i]);
        if (i + 1 >= argc) return ERRF(ResultUnit, ERR_USAGE, "Missing value for %s", argv[i]);
        TRY0(ResultUnit, spec->apply(cli, argv[i], argv[i + 1]));
    }
    return OK_UNIT(ResultUnit);
}

/* ---- output ------------------------------------------------------------ */

static ResultUnit ensure_parent_dir(const char *path) {
    char *tmp = strdup(path);
    if (!tmp) return ERRF(ResultUnit, ERR_NOMEM, "out of memory");
    char *slash = strrchr(tmp, '/');
    if (slash && slash != tmp) {
        *slash = '\0';
        for (char *p = tmp + 1; *p; p++) {
            if (*p != '/') continue;
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) goto fail;
            *p = '/';
        }
        if (mkdir(tmp, 0777) != 0 && errno != EEXIST) goto fail;
    }
    free(tmp);
    return OK_UNIT(ResultUnit);
fail:;
    Error e = error_make(ERR_IO, "Cannot create output directory %s: %s", tmp, strerror(errno));
    free(tmp);
    return ERR(ResultUnit, e);
}

static ResultUnit write_json(const char *path, const Cnf *cnf, const SolveResult *r) {
    TRY0(ResultUnit, ensure_parent_dir(path));
    FILE *f = fopen(path, "wb");
    if (!f) return ERRF(ResultUnit, ERR_IO, "Cannot write result %s: %s", path, strerror(errno));
    if (r->status == SOLVE_SAT) {
        fprintf(f, "{\n  \"status\": \"SAT\",\n  \"variables\": %zu,\n  \"clauses\": %zu,\n"
                   "  \"steps\": %zu,\n  \"restart\": %zu,\n  \"assignment\": [",
                cnf->n_vars, cnf->n_clauses, r->steps, r->restart);
        for (size_t i = 0; i < r->n_vars; i++) fprintf(f, "%s%s", i ? ", " : "", r->assignment[i] ? "true" : "false");
        fprintf(f, "]\n}\n");
    } else if (r->status == SOLVE_UNSAT) {
        fprintf(f, "{\n  \"status\": \"UNSAT\",\n  \"message\": \"UNSAT\",\n"
                   "  \"variables\": %zu,\n  \"clauses\": %zu,\n  \"contradictions\": [",
                cnf->n_vars, cnf->n_clauses);
        for (size_t i = 0; i < r->n_contradictions; i++)
            fprintf(f, "%s{\"i\": %zu, \"j\": %zu}", i ? ", " : "",
                    r->contradictions[i].a, r->contradictions[i].b);
        fprintf(f, "]\n}\n");
    } else {
        fprintf(f, "{\n  \"status\": \"UNKNOWN\",\n  \"variables\": %zu,\n  \"clauses\": %zu,\n"
                   "  \"steps\": %zu,\n  \"best_unsatisfied\": %zu\n}\n",
                cnf->n_vars, cnf->n_clauses, r->steps, r->best_unsatisfied);
    }
    if (fclose(f) != 0) return ERRF(ResultUnit, ERR_IO, "Cannot write result %s: %s", path, strerror(errno));
    return OK_UNIT(ResultUnit);
}

static void json_id_list(FILE *f, const size_t *ids, size_t n) {   /* 1-based, DIMACS style */
    fputc('[', f);
    for (size_t i = 0; i < n; i++) fprintf(f, "%s%zu", i ? ", " : "", ids[i] + 1);
    fputc(']', f);
}

static ResultUnit write_group_json(const char *path, const Cnf *cnf, const GroupResult *r) {
    TRY0(ResultUnit, ensure_parent_dir(path));
    FILE *f = fopen(path, "wb");
    if (!f) return ERRF(ResultUnit, ERR_IO, "Cannot write result %s: %s", path, strerror(errno));
    if (r->status == SOLVE_UNSAT) {
        fprintf(f, "{\n  \"status\": \"UNSAT\",\n  \"mode\": \"groups\",\n  \"variables\": %zu,\n"
                   "  \"clauses\": %zu,\n  \"contradictions\": [", cnf->n_vars, cnf->n_clauses);
        for (size_t i = 0; i < r->n_contradictions; i++)
            fprintf(f, "%s{\"i\": %zu, \"j\": %zu}", i ? ", " : "", r->contradictions[i].a, r->contradictions[i].b);
        fprintf(f, "]\n}\n");
    } else {
        fprintf(f, "{\n  \"status\": \"%s\",\n  \"mode\": \"groups\",\n  \"variables\": %zu,\n"
                   "  \"clauses\": %zu,\n  \"steps\": %zu,\n  \"groups\": %zu,\n  \"group_sizes\": [",
                r->status == SOLVE_SAT ? "SAT" : "UNKNOWN", cnf->n_vars, cnf->n_clauses, r->steps, r->n_groups);
        for (size_t i = 0; i < r->n_groups; i++) fprintf(f, "%s%zu", i ? ", " : "", r->group_sizes[i]);
        fprintf(f, "],\n  \"merged\": %s,\n  \"merge_flips\": %zu,\n  \"merge_flips_free\": %zu,\n",
                r->merged ? "true" : "false", r->merge_flips, r->merge_flips_free);
        if (r->merged) fprintf(f, "  \"anchor_group\": %zu,\n", r->anchor + 1);
        if (r->status == SOLVE_UNKNOWN) {
            /* merged part of the system: what is still broken and what can/cannot move */
            /* each leftover clause together with the group(s) it was rogue in (ids 1-based; [] = none) */
            fprintf(f, "  \"unmerged_clauses\": [");
            for (size_t i = 0; i < r->n_rogue; i++) {
                fprintf(f, "%s\n    {\"clause\": %zu, \"groups\": ", i ? "," : "", r->rogue[i] + 1);
                json_id_list(f, r->rogue_group + r->rogue_group_off[i], r->rogue_group_off[i + 1] - r->rogue_group_off[i]);
                fputc('}', f);
            }
            fprintf(f, "%s]", r->n_rogue ? "\n  " : "");
            fprintf(f, ",\n  \"neutral_vars\": ");   json_id_list(f, r->neutral, r->n_neutral);
            fprintf(f, ",\n  \"prime_vars\": ");     json_id_list(f, r->prime, r->n_prime);
            fprintf(f, ",\n  \"merged_assignment\": [");
        } else {
            fprintf(f, "  \"assignment\": [");
        }
        for (size_t i = 0; i < r->n_vars; i++) fprintf(f, "%s%s", i ? ", " : "", r->assignment[i] ? "true" : "false");
        fprintf(f, "]\n}\n");
    }
    if (fclose(f) != 0) return ERRF(ResultUnit, ERR_IO, "Cannot write result %s: %s", path, strerror(errno));
    return OK_UNIT(ResultUnit);
}

static ResultInt solve_groups_and_report(const Cnf *cnf, const Cli *cli) {
    GroupResult res = TRY(ResultInt, group_solve(cnf, &cli->cfg, &cli->gcfg));
    ResultUnit w = write_group_json(cli->output, cnf, &res);
    if (!w.ok) { group_result_free(&res); return ERR(ResultInt, w.err); }
    int code;
    if (res.status == SOLVE_SAT) {
        printf("SAT (%zu group%s, %zu steps, %zu merge flip%s) \xe2\x80\x94 %s\n", res.n_groups,
               res.n_groups == 1 ? "" : "s", res.steps, res.merge_flips, res.merge_flips == 1 ? "" : "s", cli->output);
        code = 0;
    } else if (res.status == SOLVE_UNSAT) {
        printf("UNSAT (%zu contradiction%s) \xe2\x80\x94 %s\n", res.n_contradictions,
               res.n_contradictions == 1 ? "" : "s", cli->output);
        for (size_t i = 0; i < res.n_contradictions; i++)
            printf("  clause %zu == not clause %zu\n", res.contradictions[i].b, res.contradictions[i].a);
        code = 4;
    } else {
        printf("UNKNOWN (%zu group%s, %zu merge flip%s) \xe2\x80\x94 %zu unmerged clause%s, %zu neutral, %zu prime \xe2\x80\x94 %s\n",
               res.n_groups, res.n_groups == 1 ? "" : "s", res.merge_flips, res.merge_flips == 1 ? "" : "s",
               res.n_rogue, res.n_rogue == 1 ? "" : "s", res.n_neutral, res.n_prime, cli->output);
        code = 3;
    }
    group_result_free(&res);
    return OK(ResultInt, code);
}

/* ---- run --------------------------------------------------------------- */

static ResultInt solve_and_report(const Cnf *cnf, const Cli *cli) {
    if (!cnf_is_3sat(cnf))
        fprintf(stderr, "Warning: input contains clauses longer than 3 literals; solving general CNF instead of strict 3-SAT.\n");
    if (cli->groups_mode) return solve_groups_and_report(cnf, cli);
    Solver *solver = TRY(ResultInt, solver_new(cnf, cli->cfg.seed));
    ResultSolve rs = solver_solve(solver, &cli->cfg);
    solver_free(solver);
    SolveResult res = TRY(ResultInt, rs);

    if (res.status == SOLVE_SAT && !verify(cnf, res.assignment, res.n_vars)) {
        solve_result_free(&res);
        return ERRF(ResultInt, ERR_VERIFY, "Internal verification failed");
    }
    ResultUnit w = write_json(cli->output, cnf, &res);
    if (!w.ok) { solve_result_free(&res); return ERR(ResultInt, w.err); }

    int code;
    if (res.status == SOLVE_SAT) {
        printf("SAT (%zu steps, restart %zu) \xe2\x80\x94 %s\n", res.steps, res.restart, cli->output);
        code = 0;
    } else if (res.status == SOLVE_UNSAT) {
        printf("UNSAT (%zu contradiction%s) \xe2\x80\x94 %s\n",
               res.n_contradictions, res.n_contradictions == 1 ? "" : "s", cli->output);
        for (size_t i = 0; i < res.n_contradictions; i++)
            printf("  clause %zu == not clause %zu\n", res.contradictions[i].b, res.contradictions[i].a);
        code = 4;
    } else {
        printf("UNKNOWN (%zu steps, best %zu unsatisfied) \xe2\x80\x94 %s\n", res.steps, res.best_unsatisfied, cli->output);
        code = 3;
    }
    solve_result_free(&res);
    return OK(ResultInt, code);
}

static ResultInt run(int argc, char **argv) {
    Cli cli = { .input = NULL, .output = DEFAULT_OUT, .cfg = solver_config_default(),
                .groups_mode = false, .gcfg = group_config_default() };
    TRY0(ResultInt, parse_args(argc, argv, &cli));
    Cnf cnf = TRY_CTX(ResultInt, cnf_from_dimacs_file(cli.input), "Failed to load CNF");
    ResultInt r = solve_and_report(&cnf, &cli);
    cnf_free(&cnf);
    return r;
}

int main(int argc, char **argv) {
    ResultInt r = run(argc, argv);
    if (r.ok) return r.val;
    fprintf(stderr, "%s\n", r.err.msg);
    if (r.err.kind == ERR_USAGE) { usage(); return 2; }
    return 1;
}
