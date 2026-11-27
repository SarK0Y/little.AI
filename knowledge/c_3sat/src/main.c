#include <ctype.h>
#include <errno.h>
#include <time.h>
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
    else if (strcmp(value, "glowest") == 0) cli->cfg.init = INIT_GLOWEST;
    else if (strcmp(value, "min-overlapping") == 0) cli->cfg.init = INIT_MIN_OVERLAP;
    else if (strcmp(value, "gob") == 0) cli->cfg.init = INIT_GOB;
    else if (strcmp(value, "gob-sign") == 0) cli->cfg.init = INIT_GOB_SIGN;
    else if (strcmp(value, "gob-square") == 0) cli->cfg.init = INIT_GOB_SQUARE;
    else if (strcmp(value, "gob-recult") == 0) cli->cfg.init = INIT_GOB_RECULT;
    else if (strcmp(value, "reverse-rarest") == 0) cli->cfg.init = INIT_REVERSE_RAREST;
    else if (strcmp(value, "reverse-glowest") == 0) cli->cfg.init = INIT_REVERSE_GLOWEST;
    else if (strcmp(value, "reverse-min-overlapping") == 0) cli->cfg.init = INIT_REVERSE_MIN_OVERLAP;
    else if (strcmp(value, "likho") == 0) cli->cfg.init = INIT_LIKHO;
    else if (strcmp(value, "terminator") == 0) cli->cfg.init = INIT_TERMINATOR;
    else if (strcmp(value, "baddymove") == 0) cli->cfg.init = INIT_BADDYMOVE;
    else if (strcmp(value, "cascade") == 0) cli->cfg.init = INIT_CASCADE;
    else if (strcmp(value, "neighborhood") == 0) cli->cfg.init = INIT_NEIGHBORHOOD;
    else if (strcmp(value, "neighborhood0") == 0) cli->cfg.init = INIT_NEIGHBORHOOD0;
    else if (strcmp(value, "yanus") == 0) cli->cfg.init = INIT_YANUS;
    else if (strcmp(value, "knots") == 0) cli->cfg.init = INIT_KNOTS;
    else return ERRF(ResultUnit, ERR_USAGE, "Invalid value for %s: `%s` (expected majority|rarest|glowest|min-overlapping|gob|gob-sign|gob-square|gob-recult|reverse-rarest|reverse-glowest|reverse-min-overlapping|likho|terminator|baddymove|cascade|neighborhood|neighborhood0|yanus|knots)", flag, value);
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_pre_cook(Cli *cli, const char *flag, const char *value) {
    if (strcmp(value, "zero") == 0) cli->cfg.likho_pre_cook = INIT_ZERO;
    else if (strcmp(value, "majority") == 0) cli->cfg.likho_pre_cook = INIT_MAJORITY;
    else if (strcmp(value, "rarest") == 0) cli->cfg.likho_pre_cook = INIT_RAREST;
    else if (strcmp(value, "glowest") == 0) cli->cfg.likho_pre_cook = INIT_GLOWEST;
    else if (strcmp(value, "min-overlapping") == 0) cli->cfg.likho_pre_cook = INIT_MIN_OVERLAP;
    else if (strcmp(value, "gob") == 0) cli->cfg.likho_pre_cook = INIT_GOB;
    else if (strcmp(value, "gob-sign") == 0) cli->cfg.likho_pre_cook = INIT_GOB_SIGN;
    else if (strcmp(value, "gob-square") == 0) cli->cfg.likho_pre_cook = INIT_GOB_SQUARE;
    else if (strcmp(value, "gob-recult") == 0) cli->cfg.likho_pre_cook = INIT_GOB_RECULT;
    else if (strcmp(value, "neighborhood") == 0) cli->cfg.likho_pre_cook = INIT_NEIGHBORHOOD;
    else if (strcmp(value, "neighborhood0") == 0) cli->cfg.likho_pre_cook = INIT_NEIGHBORHOOD0;
    else if (strcmp(value, "reverse-rarest") == 0) cli->cfg.likho_pre_cook = INIT_REVERSE_RAREST;
    else if (strcmp(value, "reverse-glowest") == 0) cli->cfg.likho_pre_cook = INIT_REVERSE_GLOWEST;
    else if (strcmp(value, "reverse-min-overlapping") == 0) cli->cfg.likho_pre_cook = INIT_REVERSE_MIN_OVERLAP;
    else if (strcmp(value, "knots") == 0) cli->cfg.likho_pre_cook = INIT_KNOTS;
    else return ERRF(ResultUnit, ERR_USAGE, "Invalid value for %s: `%s` (expected zero|majority|rarest|glowest|min-overlapping|gob|gob-sign|gob-square|gob-recult|neighborhood|neighborhood0|reverse-rarest|reverse-glowest|reverse-min-overlapping|knots)", flag, value);
    if (cli->cfg.init != INIT_TERMINATOR) cli->cfg.init = INIT_LIKHO;   /* pre-cook is Likho mode unless terminator */
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_group_len(Cli *cli, const char *flag, const char *value) {
    return parse_size(flag, value, &cli->gcfg.group_len);
}
static ResultUnit opt_terminator_rounds(Cli *cli, const char *flag, const char *value) {
    return parse_size(flag, value, &cli->cfg.terminator_rounds);
}
static ResultUnit opt_terminator_order(Cli *cli, const char *flag, const char *value) {
    if (strcmp(value, "forward") == 0) cli->cfg.terminator_order = TERM_FORWARD;
    else if (strcmp(value, "reverse") == 0) cli->cfg.terminator_order = TERM_REVERSE;
    else if (strcmp(value, "random") == 0) cli->cfg.terminator_order = TERM_RANDOM;
    else return ERRF(ResultUnit, ERR_USAGE, "Invalid value for %s: `%s` (expected forward|reverse|random)", flag, value);
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_neighborhood_timeout(Cli *cli, const char *flag, const char *value) {
    return parse_size(flag, value, &cli->cfg.neighborhood_timeout_ms);
}
static ResultUnit opt_neighborhood_start(Cli *cli, const char *flag, const char *value) {
    if (strcmp(value, "most") == 0) cli->cfg.neighborhood_start = NB_MOST;
    else if (strcmp(value, "least") == 0) cli->cfg.neighborhood_start = NB_LEAST;
    else if (strcmp(value, "random") == 0) cli->cfg.neighborhood_start = NB_RANDOM;
    else return ERRF(ResultUnit, ERR_USAGE, "Invalid value for %s: `%s` (expected most|least|random)", flag, value);
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_baddymove_allow(Cli *cli, const char *flag, const char *value) {
    char *end = NULL;
    double v = strtod(value, &end);
    if (end == value || *end != '\0' || v < 0.0 || v > 1.0)
        return ERRF(ResultUnit, ERR_USAGE, "Invalid value for %s: `%s` (expected float in [0,1])", flag, value);
    cli->cfg.baddymove_allow = v;
    return OK_UNIT(ResultUnit);
}
/* -deep-dive VARS NODES: cap switch domains, and fire them before neutrals. */
static ResultUnit opt_deep_dive(Cli *cli, const char *flag, const char *vars, const char *nodes) {
    TRY0(ResultUnit, parse_size(flag, vars, &cli->cfg.likho_domain_max));
    TRY0(ResultUnit, parse_size(flag, nodes, &cli->cfg.likho_search_nodes));
    if (cli->cfg.likho_domain_max < 1)
        return ERRF(ResultUnit, ERR_USAGE, "%s needs at least 1 variable per domain", flag);
    if (cli->cfg.likho_search_nodes < 1)
        return ERRF(ResultUnit, ERR_USAGE, "%s needs at least 1 search node", flag);
    cli->cfg.likho_domains_first = true;
    cli->cfg.init = INIT_LIKHO;   /* deep-dive is a Likho mode */
    return OK_UNIT(ResultUnit);
}

static ResultUnit opt_diversify(Cli *cli, const char *flag, const char *value) {
    return parse_size(flag, value, &cli->gcfg.diversify);
}

static ResultUnit opt_play_baseline(Cli *cli, const char *flag, const char *value) {
    size_t v;
    TRY0(ResultUnit, parse_size(flag, value, &v));
    if (v < 1) return ERRF(ResultUnit, ERR_USAGE, "%s must be >= 1", flag);
    cli->cfg.gob_recult_play = v;
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_set_baseline(Cli *cli, const char *flag, const char *value) {
    char *end = NULL;
    errno = 0;
    double v = strtod(value, &end);
    if (!*value || errno || *end)
        return ERRF(ResultUnit, ERR_USAGE, "Invalid value for %s: `%s`", flag, value);
    cli->cfg.gob_recult_set = v;
    cli->cfg.gob_recult_has_set = true;
    return OK_UNIT(ResultUnit);
}

static ResultUnit opt_coef_k(Cli *cli, const char *flag, const char *value) {
    size_t v;
    TRY0(ResultUnit, parse_size(flag, value, &v));
    if (v < 1) return ERRF(ResultUnit, ERR_USAGE, "%s must be >= 1", flag);
    cli->cfg.gob_recult_coef_k = v;
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_yanus_timeout(Cli *cli, const char *flag, const char *value) {
    return parse_size(flag, value, &cli->cfg.yanus_timeout_ms);
}
static ResultUnit opt_yanus_rounds(Cli *cli, const char *flag, const char *value) {
    size_t v; TRY0(ResultUnit, parse_size(flag, value, &v));
    cli->cfg.yanus_attempts = v;
    cli->cfg.knots_rounds = v;
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_yanus_saturate(Cli *cli, const char *flag, const char *value) {
    char *end = NULL;
    errno = 0;
    double v = strtod(value, &end);
    if (!*value || errno || *end || v < 0.0)
        return ERRF(ResultUnit, ERR_USAGE, "Invalid value for %s: `%s` (expected float >= 0)", flag, value);
    cli->cfg.yanus_saturate = v;
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_yanus_mode(Cli *cli, const char *flag, const char *value) {
    cli->cfg.yanus_glowest = false; cli->cfg.yanus_terminator = false; cli->cfg.yanus_baddymove = false;
    if (strcmp(value, "glowest") == 0) { cli->cfg.yanus_glowest = true; }
    else if (strcmp(value, "terminator") == 0) { cli->cfg.yanus_terminator = true; }
    else if (strcmp(value, "baddymove") == 0) { cli->cfg.yanus_baddymove = true; }
    else if (strcmp(value, "both") == 0) { cli->cfg.yanus_glowest = true; cli->cfg.yanus_terminator = true; }
    else return ERRF(ResultUnit, ERR_USAGE, "Invalid value for %s: `%s` (expected glowest|terminator|baddymove|both)", flag, value);
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_yanus_diving(Cli *cli, const char *flag, const char *value) {
    char *end = NULL; errno = 0;
    double v = strtod(value, &end);
    if (!*value || errno || *end || v < 0.0)
        return ERRF(ResultUnit, ERR_USAGE, "Invalid value for %s: `%s` (expected float >= 0)", flag, value);
    cli->cfg.yanus_diving = v;
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_knots_random_mode(Cli *cli, const char *flag, const char *value) {
    char *end = NULL; errno = 0;
    unsigned long long v = strtoull(value, &end, 10);
    if (!*value || errno || *end)
        return ERRF(ResultUnit, ERR_USAGE, "Invalid value for %s: `%s` (expected integer seed, 0 disables)", flag, value);
    cli->cfg.knots_random_mode = (v != 0);
    cli->cfg.knots_seed = (uint64_t)v;
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_knots_sort(Cli *cli, const char *flag, const char *value) {
    (void)flag;
    if (strcmp(value,"desc")==0) cli->cfg.knots_sort = KNOTS_SORT_DESC;
    else if (strcmp(value,"asc")==0) cli->cfg.knots_sort = KNOTS_SORT_ASC;
    else return ERRF(ResultUnit, ERR_USAGE, "Invalid value for %s: `%s` (expected desc|asc)", flag, value);
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_knots_saturation(Cli *cli, const char *flag, const char *value) {
    (void)flag; (void)value;
    cli->cfg.knots_sort = KNOTS_SORT_SATURATION;
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_knots_sort_dyn(Cli *cli, const char *flag, const char *value) {
    (void)flag;
    if (strcmp(value,"desc")==0) cli->cfg.knots_sort = KNOTS_SORT_DYN_DESC;
    else if (strcmp(value,"asc")==0) cli->cfg.knots_sort = KNOTS_SORT_DYN_ASC;
    else return ERRF(ResultUnit, ERR_USAGE, "Invalid value for %s: `%s` (expected desc|asc)", flag, value);
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_path_rider(Cli *cli, const char *flag, const char *value) {
    return parse_size(flag, value, &cli->cfg.path_rider);
}
static ResultUnit opt_knots_mono(Cli *cli, const char *flag, const char *value) {
    (void)flag; (void)value;
    cli->cfg.knots_mono = true;
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_knots_reverse(Cli *cli, const char *flag, const char *value) {
    (void)flag; (void)value;
    cli->cfg.knots_reverse = true;
    return OK_UNIT(ResultUnit);
}
static ResultUnit opt_knots_keep_bad(Cli *cli, const char *flag, const char *value) {
    return parse_size(flag, value, &cli->cfg.knots_keep_bad);
}
static ResultUnit opt_knots_mode(Cli *cli, const char *flag, const char *value) {
    (void)flag;
    cli->cfg.knots_glowest = false; cli->cfg.knots_terminator = false; cli->cfg.knots_likho = false; cli->cfg.knots_yanus = false; cli->cfg.knots_gob_recult = false;
    char buf[64]; strncpy(buf, value, sizeof(buf)-1); buf[sizeof(buf)-1]=0;
    for(char *p=buf; *p; p++) if(*p=='+') *p=',';
    char *tok = strtok(buf, ",");
    bool any=false;
    while(tok){
        if(strcmp(tok,"glowest")==0){ cli->cfg.knots_glowest=true; any=true; }
        else if(strcmp(tok,"terminator")==0){ cli->cfg.knots_terminator=true; any=true; }
        else if(strcmp(tok,"likho")==0){ cli->cfg.knots_likho=true; any=true; }
        else if(strcmp(tok,"yanus")==0){ cli->cfg.knots_yanus=true; any=true; }
        else if(strcmp(tok,"gob-recult")==0 || strcmp(tok,"gob_recult")==0){ cli->cfg.knots_gob_recult=true; any=true; }
        else return ERRF(ResultUnit, ERR_USAGE, "Invalid value for -knots-mode: `%s` (expected glowest|terminator|likho|yanus|gob-recult|combo)", value);
        tok=strtok(NULL,",");
    }
    if(!any) return ERRF(ResultUnit, ERR_USAGE, "Invalid value for -knots-mode: `%s`", value);
    return OK_UNIT(ResultUnit);
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
    { "--init", opt_init },           { "-pre-cook", opt_pre_cook },
    { "--pre-cook", opt_pre_cook },   { "--terminator-rounds", opt_terminator_rounds },
    { "--terminator-order", opt_terminator_order },
    { "--neighborhood-timeout", opt_neighborhood_timeout },
    { "--neighborhood-start", opt_neighborhood_start },
    { "-allow", opt_baddymove_allow }, { "--allow", opt_baddymove_allow },
    { "-play-baseline", opt_play_baseline }, { "--play-baseline", opt_play_baseline },
    { "-set-baseline", opt_set_baseline },   { "--set-baseline", opt_set_baseline },
    { "-coef-k", opt_coef_k },               { "--coef-k", opt_coef_k },
    { "-timeout", opt_yanus_timeout },       { "--timeout", opt_yanus_timeout },
    { "-rounds", opt_yanus_rounds },         { "--rounds", opt_yanus_rounds },
    { "-saturate", opt_yanus_saturate },     { "--saturate", opt_yanus_saturate },
    { "-yanus-mode", opt_yanus_mode },       { "--yanus-mode", opt_yanus_mode },
    { "-diving", opt_yanus_diving },         { "--diving", opt_yanus_diving },
    { "-random-mode", opt_knots_random_mode }, { "--random-mode", opt_knots_random_mode },
    { "-sorted", opt_knots_sort },             { "--sorted", opt_knots_sort },
    { "-sort", opt_knots_sort },               { "--sort", opt_knots_sort },
    { "-sorted-dyn", opt_knots_sort_dyn },     { "--sorted-dyn", opt_knots_sort_dyn },
    { "-path-rider", opt_path_rider },         { "--path-rider", opt_path_rider },
    { "-knots-mode", opt_knots_mode },         { "--knots-mode", opt_knots_mode },
    { "-mono", opt_knots_mono },               { "--mono", opt_knots_mono },
    { "-reverse", opt_knots_reverse },         { "--reverse", opt_knots_reverse },
    { "-keep-bad", opt_knots_keep_bad },       { "--keep-bad", opt_knots_keep_bad },
    { "-saturation", opt_knots_saturation },   { "--saturation", opt_knots_saturation },
};

static void usage(void) {
    fprintf(stderr,
        "Usage: 3sat INPUT.cnf [--steps N] [--restarts N] [--noise 0..1] [--seed N]\n"
        "                      [--strategy ranked|greedy|random] [--progress N] [--out FILE]\n"
        "                      [--mode plain|groups] [--max-groups 1..1024] [--merge free|directed]\n"
        "                      [--diversify N] [--group-len N]\n"
        "                      [--init majority|rarest|glowest|min-overlapping|gob|gob-sign|gob-square|gob-recult|reverse-rarest|reverse-glowest|reverse-min-overlapping|likho|terminator|baddymove|cascade|neighborhood|neighborhood0|yanus|knots]\n"
        "                      [--terminator-rounds N] [--terminator-order forward|reverse|random]\n"
        "                      [--neighborhood-timeout MS] [--neighborhood-start most|least|random]\n"
        "                      [-allow P]\n"
        "                      [-deep-dive VARS NODES]\n"
        "                      [-pre-cook zero|majority|rarest|glowest|min-overlapping|gob|gob-sign|gob-square|gob-recult|reverse-rarest|reverse-glowest|reverse-min-overlapping|knots]\n"
        "                      [-play-baseline P] [-set-baseline N] [-coef-k K]\n"
        "                      [-timeout MS] [-rounds N] (Yanus: default 30ms, 0 rounds)\n"
        "                      [-saturate F] (Yanus extra vars = F * n_clauses, 0 = random)\n"
        "                      [-yanus-mode glowest|terminator|baddymove|both] [-diving K]\n"
        "                      [-random-mode SEED] (Knots: random clause order, 0 disables; -rounds N attempts)\n"
        "                      [-sorted desc|asc|-sort desc|asc] (Knots: clause order by var popularity)\n"
        "                      [-sorted-dyn desc|asc] (Knots: dynamic popularity per step)\n"
        "                      [-path-rider P] (Knots: try 2^P random var-equality assignments)\n"
        "                      [-knots-mode glowest|terminator|likho|yanus|gob-recult|combo] (Knots post-process)\n"
        "                      [-mono] (Knots: block diagnostics on fail)\n"
        "                      [-reverse] (Knots: negate every literal in each clause)\n"
        "                      [-keep-bad S] (Knots: keep bad variant for S hits before killing)\n"
        "                      [-saturation] (Knots: process most-saturated mono blocks first)\n");
}

static ResultUnit parse_args(int argc, char **argv, Cli *cli) {
    if (argc < 2) return ERRF(ResultUnit, ERR_USAGE, "Missing INPUT.cnf");
    cli->input = argv[1];
    for (int i = 2; i < argc; i += 2) {
        if (strcmp(argv[i], "-deep-dive") == 0 || strcmp(argv[i], "--deep-dive") == 0) {
            if (i + 2 >= argc) return ERRF(ResultUnit, ERR_USAGE, "Missing values for %s", argv[i]);
            TRY0(ResultUnit, opt_deep_dive(cli, argv[i], argv[i + 1], argv[i + 2]));
            i += 1;   /* consumed two values; the loop's step lands on the next flag */
            continue;
        }
        const OptSpec *spec = NULL;
        for (size_t k = 0; k < sizeof OPTS / sizeof OPTS[0]; k++)
            if (strcmp(OPTS[k].flag, argv[i]) == 0) { spec = &OPTS[k]; break; }
        if (!spec) return ERRF(ResultUnit, ERR_USAGE, "Unknown option: %s", argv[i]);
        if (spec->apply == opt_knots_mono || spec->apply == opt_knots_reverse || spec->apply == opt_knots_saturation) { TRY0(ResultUnit, spec->apply(cli, argv[i], "1")); i -= 1; continue; }
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
                   "  \"steps\": %zu,\n  \"best_unsatisfied\": %zu,\n  \"assignment\": [",
                cnf->n_vars, cnf->n_clauses, r->steps, r->best_unsatisfied);
        for (size_t i = 0; i < r->n_vars; i++) fprintf(f, "%s%s", i ? ", " : "", r->assignment[i] ? "true" : "false");
        fprintf(f, "]\n}\n");
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
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    ResultSolve rs = solver_solve(solver, &cli->cfg);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    solver_free(solver);
    SolveResult res = TRY(ResultInt, rs);
    double elapsed = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    if (res.status == SOLVE_SAT && !verify(cnf, res.assignment, res.n_vars)) {
        solve_result_free(&res);
        return ERRF(ResultInt, ERR_VERIFY, "Internal verification failed");
    }
    ResultUnit w = write_json(cli->output, cnf, &res);
    if (!w.ok) { solve_result_free(&res); return ERR(ResultInt, w.err); }

    int code;
    if (res.status == SOLVE_SAT) {
        printf("SAT (%zu steps, restart %zu, %.3fs) \xe2\x80\x94 %s\n", res.steps, res.restart, elapsed, cli->output);
        code = 0;
    } else if (res.status == SOLVE_UNSAT) {
        printf("UNSAT (%zu contradiction%s, %.3fs) \xe2\x80\x94 %s\n",
               res.n_contradictions, res.n_contradictions == 1 ? "" : "s", elapsed, cli->output);
        for (size_t i = 0; i < res.n_contradictions; i++)
            printf("  clause %zu == not clause %zu\n", res.contradictions[i].b, res.contradictions[i].a);
        code = 4;
    } else {
        printf("UNKNOWN (%zu steps, best %zu unsatisfied, %.3fs) \xe2\x80\x94 %s\n", res.steps, res.best_unsatisfied, elapsed, cli->output);
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
