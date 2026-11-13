/* context-3sat (C port): incremental 3-SAT stochastic local search.
 *
 * Returns SAT when it finds a verified satisfying assignment and UNKNOWN when
 * the budget ends. It never claims UNSAT (local search cannot prove it).
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

typedef struct {
    size_t max_steps;
    size_t restarts;
    double noise;     /* probability of a random pick among top candidates */
    uint64_t seed;
    PickFn pick;      /* NULL => pick_ranked */
    ProgressFn on_progress;
    void *progress_ctx;
    size_t progress_every;
} SolverConfig;

SolverConfig solver_config_default(void);

/* ---- solver ----------------------------------------------------------- */

typedef struct Solver Solver;   /* borrows the Cnf: keep it alive */
typedef RESULT(Solver *) ResultSolver;

typedef enum { SOLVE_SAT, SOLVE_UNKNOWN } SolveStatus;
typedef struct {
    SolveStatus status;
    bool *assignment;        /* owned; SAT only (else NULL) */
    size_t n_vars;
    size_t steps;
    size_t restart;          /* SAT only */
    size_t best_unsatisfied; /* UNKNOWN only */
} SolveResult;
typedef RESULT(SolveResult) ResultSolve;

ResultSolver solver_new(const Cnf *cnf, uint64_t seed);
void solver_free(Solver *s);
ResultSolve solver_solve(Solver *s, const SolverConfig *cfg);
void solve_result_free(SolveResult *r);

/* Introspection (also used by the tests). */
const bool *solver_assignment(const Solver *s);
size_t solver_unsatisfied_count(const Solver *s);
bool solver_clause_is_rogue(const Solver *s, size_t clause);
size_t solver_clause_support(const Solver *s, size_t clause);
const VarStats *solver_var_stats(Solver *s);  /* refreshes lazily if stale */
void solver_set_assignment(Solver *s, const bool *assignment);
void solver_flip(Solver *s, size_t var);

#endif
