/* context-3sat (C port): incremental 3-SAT stochastic local search.
 *
 * Returns SAT when it finds a verified satisfying assignment and UNKNOWN when
 * the budget ends. Local search cannot prove UNSAT, but a contradictory unit
 * pair -- `(x)` and `(-x)` -- is checked up front and reported as UNSAT.
 */
#ifndef C3SAT_SAT_H
#define C3SAT_SAT_H

#include <stddef.h>
#include <stdint.h>
#include "result.h"

/* ---- CNF -------------------------------------------------------------- */

typedef struct {
    uint32_t var;   /* 0-based */
    bool positive;
} Lit;

/* Flat (CSR) layout: clause i is lits[off[i] .. off[i+1]). Owns its memory. */
typedef struct {
    size_t n_vars;
    size_t n_clauses;
    size_t *off;    /* n_clauses + 1 entries */
    Lit *lits;
} Cnf;

typedef RESULT(Cnf) ResultCnf;

ResultCnf cnf_from_dimacs_str(const char *input);
ResultCnf cnf_from_dimacs_file(const char *path);
void cnf_free(Cnf *cnf);
/* Build a clause-system copy with synchronous and asynchronous twin clauses removed.
 * Clause literal order is ignored; async twins invert every literal. Keeps the first. */
bool no_twins(const Cnf *src, Cnf *out);
/* Copy src with every literal negated (the "vice versa" reversed system). */
bool cnf_negate(const Cnf *src, Cnf *out);
bool cnf_is_3sat(const Cnf *cnf);
static inline size_t cnf_clause_len(const Cnf *c, size_t i) { return c->off[i + 1] - c->off[i]; }
static inline const Lit *cnf_clause(const Cnf *c, size_t i) { return c->lits + c->off[i]; }
static inline bool lit_value(Lit l, bool assignment) { return assignment == l.positive; }

bool verify(const Cnf *cnf, const bool *assignment, size_t len);

/* ---- RNG (xorshift64) ------------------------------------------------- */

typedef struct { uint64_t state; } Rng;

static inline Rng rng_new(uint64_t seed) {
    return (Rng){ seed ? seed : 0xa0761d6478bd642fULL };
}
static inline uint64_t rng_next_u64(Rng *r) {
    uint64_t x = r->state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return r->state = x;
}
static inline bool rng_bool(Rng *r) { return rng_next_u64(r) & 1; }
static inline double rng_f64(Rng *r) { return (double)(rng_next_u64(r) >> 11) / 9007199254740992.0; }
static inline size_t rng_index(Rng *r, size_t upper) { return (size_t)rng_next_u64(r) % upper; }

/* ---- states ----------------------------------------------------------- */

/* Rogue is deliberately NOT a variable state: only clauses can be rogue. */
typedef enum { VAR_UNUSED, VAR_PRIME, VAR_NEUTRAL } VarState;

typedef struct {
    VarState state;
    size_t min_support;            /* meaningful for VAR_NEUTRAL */
    size_t satisfied_occurrences;  /* clauses where this var's literal is true */
    size_t rogue_occurrences;      /* incidence count only; never makes the var rogue */
    size_t break_count;
    size_t make_count;
    size_t rank;                   /* position among the last ranked candidates */
} VarStats;

/* One flip candidate from a rogue clause, already scored. */
typedef struct {
    size_t var;
    uint8_t cls;      /* 0 neutral, 1 prime, 2 unused */
    size_t support;
    size_t breaks;
    size_t makes;
} Candidate;

/* ---- function-as-variable hooks --------------------------------------- */

/* Choose which variable to flip from n >= 1 candidates sorted best-first. */
typedef size_t (*PickFn)(Rng *rng, const Candidate *cands, size_t n, double noise);
/* Optional observer, called every `progress_every` flips. */
typedef void (*ProgressFn)(void *ctx, size_t steps, size_t unsatisfied);

size_t pick_ranked(Rng *rng, const Candidate *c, size_t n, double noise); /* default */
size_t pick_greedy(Rng *rng, const Candidate *c, size_t n, double noise); /* noise-free */
size_t pick_random(Rng *rng, const Candidate *c, size_t n, double noise); /* uniform */

typedef struct { const char *name; PickFn fn; } Strategy;
extern const Strategy STRATEGIES[];
extern const size_t N_STRATEGIES;
PickFn pick_fn_by_name(const char *name); /* NULL if unknown */

/* Where restart 0 starts from (later restarts are random). */
typedef enum {
    INIT_MAJORITY,  /* every variable takes its majority polarity (default) */
    INIT_RAREST,    /* constructive: rarest variables first, see solver_init_rarest() */
    INIT_GLOWEST,   /* constructive: most frequent variables first, see solver_init_glowest() */
    INIT_MIN_OVERLAP, /* constructive: most frequent first, then least-overlapping, see solver_init_min_overlap() */
    INIT_GOB,       /* linear relaxation of the clause system, see solver_init_gob() */
    INIT_GOB_SIGN,  /* the same system decoded by sign, see solver_init_gob_sign() */
    INIT_GOB_SQUARE, /* square subsystem of that system, see solver_init_gob_square() */
    INIT_GOB_RECULT, /* gob with tail-variable recultivation, see solver_init_gob_recult() */
    INIT_REVERSE_RAREST, /* negate CNF, run rarest, negate result */
    INIT_REVERSE_GLOWEST, /* negate CNF, run glowest, negate result */
    INIT_REVERSE_MIN_OVERLAP, /* negate CNF, run min-overlapping, negate result */
    INIT_LIKHO,     /* pre-cooked start (all false by default), then a one-flip-per-variable repair pass */
    INIT_TERMINATOR, /* majority start, then rounds of flips where make > break */
    INIT_BADDYMOVE, /* like terminator, but randomly allows bad flips with -allow prob */
    INIT_CASCADE,   /* ratio M/m vs _M/_m assignment by clause/alternative counts */
    INIT_NEIGHBORHOOD, /* light popularity/backtracking neighborhood search */
    INIT_NEIGHBORHOOD0, /* full neighborhood search per spec */
    INIT_YANUS,     /* random x = y_a OR y_b expansions, glowest, map back */
    INIT_KNOTS,     /* pool-based clause solver, see notes/knots.txt */
    INIT_ZERO       /* all false; Likho's phase 1, not selectable via --init */
} InitMode;

typedef enum { KNOTS_SORT_NONE = 0, KNOTS_SORT_DESC, KNOTS_SORT_ASC, KNOTS_SORT_DYN_DESC, KNOTS_SORT_DYN_ASC } KnotsSort;

/* Variable visit order for Terminator rounds. */
typedef enum { TERM_FORWARD, TERM_REVERSE, TERM_RANDOM } TerminatorOrder;
/* Start-var strategy for Neighborhood0. */
typedef enum { NB_MOST, NB_LEAST, NB_RANDOM } NeighborhoodStart;

typedef struct {
    size_t max_steps;
    size_t restarts;
    double noise;     /* probability of a random pick among top candidates */
    uint64_t seed;
    PickFn pick;      /* NULL => pick_ranked */
    const bool *initial;  /* optional start for restart 0 (n_vars entries); NULL => use `init` */
    InitMode init;
    size_t likho_domain_max;    /* Likho: variables per switch domain, prime included */
    size_t likho_search_nodes;  /* Likho: backtracking budget per prime */
    bool likho_domains_first;   /* Likho: fire switch domains before neutral repairs */
    InitMode likho_pre_cook;    /* Likho phase 1: INIT_ZERO, INIT_MAJORITY, INIT_RAREST, INIT_GLOWEST, INIT_MIN_OVERLAP, INIT_GOB, INIT_GOB_SIGN, INIT_GOB_SQUARE or INIT_GOB_RECULT */
    size_t terminator_rounds;   /* Terminator: max improvement passes (each flip makes > breaks) */
    TerminatorOrder terminator_order; /* Terminator: variable visit order per round */
    size_t neighborhood_timeout_ms; /* Neighborhood: backtracking time budget (0 = no limit) */
    NeighborhoodStart neighborhood_start; /* Neighborhood0: where to pick first var from */
    double baddymove_allow;     /* BaddyMove: probability [0..1] of taking a bad (break>make) flip */
    /* Gob-recult baseline: sum(Y_i) target. play > 0 => rng mod play; set wins if both given. */
    size_t gob_recult_play;     /* -play-baseline P: baseline = rng mod P (0 = off) */
    double gob_recult_set;      /* -set-baseline N: baseline = N */
    bool gob_recult_has_set;    /* true when -set-baseline was given */
    size_t gob_recult_coef_k;   /* -coef-k K: auto baseline range [-K*|crude|, K*|crude|] */
    size_t yanus_attempts;      /* -rounds N: random expansion attempts (0 = unlimited until timeout) */
    size_t yanus_timeout_ms;    /* -timeout MS: wall-clock budget (default 30ms) */
    double yanus_saturate;      /* -saturate F: extra vars D = F * n_clauses (0 = random) */
    bool yanus_glowest;         /* run glowest on expanded CNF (default true) */
    bool yanus_terminator;      /* hill-climb mapped assignment (default true) */
    bool yanus_baddymove;       /* hill-climb with bad moves (default false) */
    double yanus_diving;        /* -diving k: NNV[0]=D*k, NNV[i]=NNV[i-1]*k added; 0=off */
    bool knots_random_mode;     /* Knots: pick clauses randomly */
    uint64_t knots_seed;        /* Knots: RNG seed for -random-mode */
    size_t knots_rounds;        /* Knots: random-mode attempts, 0 = default 1 */
    KnotsSort knots_sort;       /* Knots: order clauses by var popularity */
    bool knots_glowest;         /* Knots: apply glowest after knots */
    bool knots_terminator;      /* Knots: apply terminator after knots */
    bool knots_likho;           /* Knots: apply likho after knots */
    bool knots_yanus;           /* Knots: apply yanus after knots */
    bool knots_gob_recult;      /* Knots: apply gob-recult after knots */
    bool knots_mono;            /* Knots: mono-block diagnostics */
    size_t path_rider;          /* -path-rider P: enumerate 2^P var assignments */
    ProgressFn on_progress;
    void *progress_ctx;
    size_t progress_every;
} SolverConfig;

SolverConfig solver_config_default(void);

/* ---- solver ----------------------------------------------------------- */

typedef struct Solver Solver;   /* borrows the Cnf: keep it alive */
typedef RESULT(Solver *) ResultSolver;

/* Two unit clauses with opposite signs: clause b == not clause a. */
typedef struct { size_t a, b; } Contradiction;

typedef enum { SOLVE_SAT, SOLVE_UNKNOWN, SOLVE_UNSAT } SolveStatus;
typedef struct {
    SolveStatus status;
    bool *assignment;        /* owned; SAT only (else NULL) */
    size_t n_vars;
    size_t steps;
    size_t restart;          /* SAT only */
    size_t best_unsatisfied; /* UNKNOWN only */
    Contradiction *contradictions; /* owned; UNSAT only (else NULL) */
    size_t n_contradictions;
} SolveResult;
typedef RESULT(SolveResult) ResultSolve;

ResultSolver solver_new(const Cnf *cnf, uint64_t seed);
void solver_free(Solver *s);
const Cnf *solver_cnf(const Solver *s);
ResultSolve solver_solve(Solver *s, const SolverConfig *cfg);
/* Contradictory unit pairs (x)/(-x); false only on allocation failure. Caller frees *out. */
bool cnf_find_contradictions(const Cnf *cnf, Contradiction **out, size_t *out_n);
void solve_result_free(SolveResult *r);

/* Introspection (also used by the tests). */
const bool *solver_assignment(const Solver *s);
size_t solver_unsatisfied_count(const Solver *s);
bool solver_clause_is_rogue(const Solver *s, size_t clause);
size_t solver_clause_support(const Solver *s, size_t clause);
const VarStats *solver_var_stats(Solver *s);  /* refreshes lazily if stale */
void solver_set_assignment(Solver *s, const bool *assignment);
void solver_flip(Solver *s, size_t var);
/* Deterministic constructive start (no RNG). Repeatedly takes the rarest unset
 * variable: fewest clauses holding x or !x (the larger of the two polarity counts,
 * satisfied clauses excluded, ties by id), recomputed after each assignment so a
 * satisfied clause drops its overlap from every remaining variable. A variable takes the value that
 *   1. satisfies more *forced* clauses (open clauses where it is the last unset variable),
 *   2. else satisfies more open (not yet satisfied) clauses,
 *   3. else has the larger total polarity, else false.
 * false only on allocation failure. */
bool solver_init_rarest(Solver *s);
/* Same constructive start as solver_init_rarest(), but takes the most frequent
 * unset variable first (most clauses holding x or !x, the larger of the two
 * counts, satisfied clauses excluded, ties by id). Deterministic, no RNG.
 * false only on allocation failure. */
bool solver_init_glowest(Solver *s);
/* Yanus: each attempt replaces original variables by fresh pairs in the signed
 * equation sense (v -> a+b), runs glowest on the expanded CNF and maps back by
 * v = a XOR b. Keeps the mapped assignment with the fewest rogue clauses;
 * stops early on SAT. cfg->yanus_attempts=0 and timeout=0 falls back to glowest. */
bool solver_init_yanus(Solver *s, const SolverConfig *cfg);
/* Knots: pool-based solver. true on SAT/assignment, false on logical fail or OOM. */
bool solver_init_knots(Solver *s, const SolverConfig *cfg);
/* Same constructive value rule, but the order is: first the most frequent
 * variable (glowest's pick, ties by id), then at every step the unset variable
 * with the smallest share of its occurrences already in satisfied clauses
 * (ties by id, variables with no occurrences last). Deterministic, no RNG.
 * false only on allocation failure. */
bool solver_init_min_overlap(Solver *s);
/* Linear start: each clause is the signed equation sum(+-x) = 1, solved by
 * Kaczmarz sweeps; each value is shifted non-negative, rounded and taken mod 2,
 * then a greedy pass flips bits that satisfy more rogue clauses than they break.
 * Deterministic, no RNG. false only on allocation failure. */
bool solver_init_gob(Solver *s);
/* The same system and repair, decoded by sign (positive = true) instead of by
 * magnitude parity. Deterministic, no RNG. false only on allocation failure. */
bool solver_init_gob_sign(Solver *s);
/* What the square subsystem's solution does to the equations it was not built
 * from: phase 3 of solver_init_gob_square(). */
typedef struct {
    size_t selected_bad;   /* selected (square) equations the real solution misses */
    size_t other_bad;      /* unselected equations the real solution misses */
    size_t other_total;    /* unselected equations checked */
} GobSquareCheck;
/* Gob over a square subsystem: pick n of the m equations (strided, one every
 * m/n clauses), solve just those by Kaczmarz, report what the solution does to
 * the rest, then decode by sign and repair like solver_init_gob_sign(). The rows can
 * be dependent and the values are fractional, so this is a seed, not a solve.
 * `check` may be NULL. Deterministic, no RNG. false only on allocation failure. */
bool solver_init_gob_square(Solver *s, GobSquareCheck *check);
/* Gob recultivation: augment clause equations with tail variables Y on the
 * right, solve by Kaczmarz (random k), decode x by sign and repair (drop Y).
 * cfg controls the tail-sum baseline (sum Y_i); NULL => 0. */
bool solver_init_gob_recult(Solver *s, const SolverConfig *cfg);
/* Vice versa: negate every clause, run init on the reversed system, negate the
 * assignment back. Supported inits: INIT_RAREST, INIT_GLOWEST, INIT_MIN_OVERLAP. */
bool solver_init_reverse(Solver *s, InitMode init);
/* Terminator: run up to `rounds` passes; in each pass visit variables in
 * `order` and flip when make_count > break_count (strictly improving). */
void solver_terminator(Solver *s, size_t rounds, TerminatorOrder order);
/* Majority start followed by Terminator rounds. */
void solver_init_terminator(Solver *s, size_t rounds, TerminatorOrder order);
/* Pre-cook then Terminator rounds (uses cfg->likho_pre_cook/terminator_*). */
bool solver_init_terminator_pre(Solver *s, const SolverConfig *cfg);
/* BaddyMove: terminator-like rounds, but bad moves (break>make) allowed with cfg->baddymove_allow. */
void solver_baddymove(Solver *s, size_t rounds, TerminatorOrder order, double allow);
bool solver_init_baddymove(Solver *s, const SolverConfig *cfg);
/* Cascade: for each var, compare sat-clause/alternative ratio for x vs !x. */
void solver_cascade(Solver *s);
/* Neighborhood: popularity-ordered var growth with neighborhood completion and
 * timed backtracking. Returns false only on allocation failure. */
bool solver_init_neighborhood(Solver *s, const SolverConfig *cfg);
/* Neighborhood0: full spec variant (popularity sort, neighbor propagation,
 * backtrack to neighbor id-1/id-2..., timeout). */
bool solver_init_neighborhood0(Solver *s, const SolverConfig *cfg);
void solver_init_majority(Solver *s);                  /* majority-polarity start (ties use the solver RNG) */
/* Likho: phase 1 is cfg->likho_pre_cook (all-false by default), then a repair
 * pass that flips an unflipped neutral variable in a rogue clause (always
 * safe), or a switch domain: a prime plus the neutrals/primes that must flip
 * with it so no satisfied clause breaks. Each variable is flipped at most
 * once. The domain size, search budget, pass order and pre-cook come from cfg
 * (NULL uses the defaults). false only on allocation failure. */
bool solver_likho(Solver *s, const SolverConfig *cfg);
const size_t *solver_rogue_list(const Solver *s);      /* solver_unsatisfied_count() clause ids */
VarStats solver_refresh_var(Solver *s, size_t var);    /* fresh stats of one variable, O(occurrences) */
size_t solver_occ_count(const Solver *s, size_t var);  /* clauses containing var */
size_t solver_occ_clause(const Solver *s, size_t var, size_t i);

#endif
