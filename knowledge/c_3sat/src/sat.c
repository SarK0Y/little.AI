#include "sat.h"

#include <errno.h>
#include <stdlib.h>

/* ======================================================================== */
/* small helpers                                                            */
/* ======================================================================== */

static bool vec_reserve(void **data, size_t *cap, size_t elem, size_t need) {
    if (need <= *cap) return true;
    size_t ncap = *cap ? *cap : 16;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2) return false;
        ncap *= 2;
    }
    if (ncap > SIZE_MAX / elem) return false;
    void *p = realloc(*data, ncap * elem);
    if (!p) return false;
    *data = p;
    *cap = ncap;
    return true;
}

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

/* Strict integer parsing (optional sign, digits only), like Rust's parse(). */
static bool parse_u64(const char *s, size_t len, uint64_t *out, bool allow_sign, bool *neg) {
    size_t i = 0;
    *neg = false;
    if (allow_sign && i < len && (s[i] == '+' || s[i] == '-')) { *neg = s[i] == '-'; i++; }
    else if (!allow_sign && i < len && s[i] == '+') i++;
    if (i == len) return false;
    uint64_t acc = 0;
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        unsigned d = (unsigned)(s[i] - '0');
        if (acc > (UINT64_MAX - d) / 10) return false;
        acc = acc * 10 + d;
    }
    *out = acc;
    return true;
}

static bool parse_i64(const char *s, size_t len, int64_t *out) {
    uint64_t mag; bool neg;
    if (!parse_u64(s, len, &mag, true, &neg)) return false;
    if (neg) {
        if (mag > (uint64_t)INT64_MAX + 1) return false;
        *out = (int64_t)(0 - mag);
    } else {
        if (mag > (uint64_t)INT64_MAX) return false;
        *out = (int64_t)mag;
    }
    return true;
}

static bool parse_usize(const char *s, size_t len, size_t *out) {
    uint64_t mag; bool neg;
    if (!parse_u64(s, len, &mag, false, &neg) || mag > SIZE_MAX) return false;
    *out = (size_t)mag;
    return true;
}

/* ======================================================================== */
/* CNF                                                                      */
/* ======================================================================== */

void cnf_free(Cnf *cnf) {
    free(cnf->off);
    free(cnf->lits);
    cnf->off = NULL;
    cnf->lits = NULL;
    cnf->n_vars = cnf->n_clauses = 0;
}

bool cnf_is_3sat(const Cnf *cnf) {
    for (size_t i = 0; i < cnf->n_clauses; i++)
        if (cnf_clause_len(cnf, i) > 3) return false;
    return true;
}

ResultCnf cnf_from_dimacs_str(const char *input) {
    Error err = {0};
    int64_t *tok = NULL; size_t ntok = 0, captok = 0;
    Lit *cur = NULL; size_t ncur = 0, capcur = 0;
    size_t capoff = 0, caplit = 0, nlit = 0;
    Cnf cnf = {0};
    size_t n_vars = 0, expected = 0;
    bool have_header = false;

#define FAIL(kind, ...) do { err = error_make((kind), __VA_ARGS__); goto fail; } while (0)

    /* Pass 1: header + raw integer tokens. */
    size_t line_no = 0;
    const char *p = input;
    while (*p) {
        line_no++;
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        const char *line = p;
        p = eol ? eol + 1 : p + llen;
        while (llen && is_ws(*line)) { line++; llen--; }
        while (llen && is_ws(line[llen - 1])) llen--;
        if (llen == 0 || line[0] == 'c') continue;

        if (line[0] == 'p') {
            struct { const char *s; size_t n; } f[5];
            size_t nf = 0, i = 0;
            while (i < llen) {
                while (i < llen && is_ws(line[i])) i++;
                if (i >= llen) break;
                size_t st = i;
                while (i < llen && !is_ws(line[i])) i++;
                if (nf < 5) { f[nf].s = line + st; f[nf].n = i - st; }
                nf++;
            }
            if (nf != 4 || f[1].n != 3 || memcmp(f[1].s, "cnf", 3) != 0)
                FAIL(ERR_PARSE, "line %zu: expected `p cnf VARS CLAUSES`", line_no);
            if (!parse_usize(f[2].s, f[2].n, &n_vars))
                FAIL(ERR_PARSE, "line %zu: invalid variable count", line_no);
            if (!parse_usize(f[3].s, f[3].n, &expected))
                FAIL(ERR_PARSE, "line %zu: invalid clause count", line_no);
            have_header = true;
            continue;
        }

        size_t i = 0;
        while (i < llen) {
            while (i < llen && is_ws(line[i])) i++;
            if (i >= llen) break;
            size_t st = i;
            while (i < llen && !is_ws(line[i])) i++;
            int64_t v;
            if (!parse_i64(line + st, i - st, &v))
                FAIL(ERR_PARSE, "line %zu: invalid DIMACS token `%.*s`", line_no, (int)(i - st), line + st);
            if (!vec_reserve((void **)&tok, &captok, sizeof *tok, ntok + 1)) FAIL(ERR_NOMEM, "out of memory");
            tok[ntok++] = v;
        }
    }
    if (!have_header) FAIL(ERR_PARSE, "missing `p cnf` header");
    if (n_vars > UINT32_MAX) FAIL(ERR_PARSE, "variable count %zu is too large", n_vars);

    /* Pass 2: build clauses; drop duplicate literals and tautologies. */
    cnf.n_vars = n_vars;
    if (!vec_reserve((void **)&cnf.off, &capoff, sizeof *cnf.off, 1)) FAIL(ERR_NOMEM, "out of memory");
    cnf.off[0] = 0;

    for (size_t t = 0; t < ntok; t++) {
        int64_t value = tok[t];
        if (value != 0) {
            if (value == INT64_MIN) FAIL(ERR_PARSE, "literal integer overflow");
            uint64_t abs = value < 0 ? (uint64_t)(-value) : (uint64_t)value;
            if (abs > n_vars) FAIL(ERR_PARSE, "literal %lld is outside 1..=%zu", (long long)value, n_vars);
            if (!vec_reserve((void **)&cur, &capcur, sizeof *cur, ncur + 1)) FAIL(ERR_NOMEM, "out of memory");
            cur[ncur++] = (Lit){ (uint32_t)(abs - 1), value > 0 };
            continue;
        }
        size_t start = cnf.off[cnf.n_clauses], appended = 0;
        bool taut = false;
        for (size_t c = 0; c < ncur && !taut; c++) {
            bool dup = false;
            for (size_t k = 0; k < appended; k++) {
                Lit e = cnf.lits[start + k];
                if (e.var != cur[c].var) continue;
                if (e.positive != cur[c].positive) taut = true; else dup = true;
                break;
            }
            if (taut || dup) continue;
            if (!vec_reserve((void **)&cnf.lits, &caplit, sizeof *cnf.lits, start + appended + 1)) FAIL(ERR_NOMEM, "out of memory");
            cnf.lits[start + appended++] = cur[c];
        }
        ncur = 0;
        if (taut) continue;
        if (!vec_reserve((void **)&cnf.off, &capoff, sizeof *cnf.off, cnf.n_clauses + 2)) FAIL(ERR_NOMEM, "out of memory");
        cnf.off[++cnf.n_clauses] = start + appended;
        nlit = start + appended;
    }
    (void)nlit;
    if (ncur != 0) FAIL(ERR_PARSE, "unterminated clause: missing 0");
    if (cnf.n_clauses > expected)
        FAIL(ERR_PARSE, "header declares %zu clauses, parsed %zu", expected, cnf.n_clauses);

    free(tok);
    free(cur);
    if (!cnf.lits) cnf.lits = malloc(sizeof *cnf.lits); /* keep non-NULL for empty formulas */
    return OK(ResultCnf, cnf);

fail:
    free(tok);
    free(cur);
    cnf_free(&cnf);
    return ERR(ResultCnf, err);
#undef FAIL
}

ResultCnf cnf_from_dimacs_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return ERRF(ResultCnf, ERR_IO, "cannot open %s: %s", path, strerror(errno));
    char *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (!vec_reserve((void **)&buf, &cap, 1, len + 4097)) { free(buf); fclose(f); return ERRF(ResultCnf, ERR_NOMEM, "out of memory"); }
        size_t n = fread(buf + len, 1, 4096, f);
        len += n;
        if (n < 4096) break;
    }
    bool bad = ferror(f);
    fclose(f);
    if (bad) { free(buf); return ERRF(ResultCnf, ERR_IO, "cannot read %s", path); }
    buf[len] = '\0';
    ResultCnf r = cnf_from_dimacs_str(buf);
    free(buf);
    return r;
}

bool verify(const Cnf *cnf, const bool *assignment, size_t len) {
    if (len < cnf->n_vars) return false;
    for (size_t i = 0; i < cnf->n_clauses; i++) {
        const Lit *c = cnf_clause(cnf, i);
        size_t n = cnf_clause_len(cnf, i);
        bool any = false;
        for (size_t k = 0; k < n && !any; k++) any = lit_value(c[k], assignment[c[k].var]);
        if (!any) return false;
    }
    return true;
}

/* ======================================================================== */
/* pick strategies (the function-as-variable part)                          */
/* ======================================================================== */

size_t pick_ranked(Rng *rng, const Candidate *c, size_t n, double noise) {
    size_t neutral = 0;
    while (neutral < n && c[neutral].cls == 0) neutral++;
    size_t pool = neutral > 0 ? neutral : n;
    if (rng_f64(rng) < noise) return c[rng_index(rng, pool)].var;
    return c[0].var;
}

size_t pick_greedy(Rng *rng, const Candidate *c, size_t n, double noise) {
    (void)rng; (void)n; (void)noise;
    return c[0].var;
}

size_t pick_random(Rng *rng, const Candidate *c, size_t n, double noise) {
    (void)noise;
    return c[rng_index(rng, n)].var;
}

const Strategy STRATEGIES[] = {
    { "ranked", pick_ranked },
    { "greedy", pick_greedy },
    { "random", pick_random },
};
const size_t N_STRATEGIES = sizeof STRATEGIES / sizeof STRATEGIES[0];

PickFn pick_fn_by_name(const char *name) {
    for (size_t i = 0; i < N_STRATEGIES; i++)
        if (strcmp(STRATEGIES[i].name, name) == 0) return STRATEGIES[i].fn;
    return NULL;
}

SolverConfig solver_config_default(void) {
    return (SolverConfig){
        .max_steps = 100000, .restarts = 20, .noise = 0.22,
        .seed = 0x9e3779b97f4a7c15ULL, .pick = pick_ranked,
        .on_progress = NULL, .progress_ctx = NULL, .progress_every = 0,
    };
}

/* ======================================================================== */
/* solver                                                                   */
/* ======================================================================== */

typedef struct { uint32_t clause; bool positive; } Occ;

struct Solver {
    const Cnf *cnf;
    bool *assignment;
    uint32_t *sat;          /* currently-true literal count per clause (0 == rogue) */
    size_t *occ_off;        /* CSR occurrence index, n_vars + 1 entries */
    Occ *occ;
    size_t *rogue;          /* clause ids currently rogue, for O(1) selection */
    size_t n_rogue;
    size_t *rogue_pos;      /* SIZE_MAX when not rogue */
    VarStats *stats;
    bool stats_dirty;       /* variable states are derived lazily */
    Candidate *cands;
    size_t max_len;
    Rng rng;
};

static size_t occ_count(const Solver *s, size_t v) { return s->occ_off[v + 1] - s->occ_off[v]; }

void solver_free(Solver *s) {
    if (!s) return;
    free(s->assignment); free(s->sat); free(s->occ_off); free(s->occ);
    free(s->rogue); free(s->rogue_pos); free(s->stats); free(s->cands);
    free(s);
}

static void add_rogue(Solver *s, size_t c) {
    if (s->rogue_pos[c] != SIZE_MAX) return;
    s->rogue_pos[c] = s->n_rogue;
    s->rogue[s->n_rogue++] = c;
}

static void remove_rogue(Solver *s, size_t c) {
    size_t pos = s->rogue_pos[c];
    if (pos == SIZE_MAX) return;
    size_t last = s->rogue[--s->n_rogue];
    if (pos < s->n_rogue) { s->rogue[pos] = last; s->rogue_pos[last] = pos; }
    s->rogue_pos[c] = SIZE_MAX;
}

static void rebuild_counts(Solver *s) {
    const Cnf *cnf = s->cnf;
    s->n_rogue = 0;
    for (size_t ci = 0; ci < cnf->n_clauses; ci++) {
        const Lit *c = cnf_clause(cnf, ci);
        size_t n = cnf_clause_len(cnf, ci), count = 0;
        for (size_t k = 0; k < n; k++) count += lit_value(c[k], s->assignment[c[k].var]);
        s->sat[ci] = (uint32_t)count;
        s->rogue_pos[ci] = SIZE_MAX;
    }
    for (size_t ci = 0; ci < cnf->n_clauses; ci++)
        if (s->sat[ci] == 0) add_rogue(s, ci);
    s->stats_dirty = true;
}

ResultSolver solver_new(const Cnf *cnf, uint64_t seed) {
    if (cnf->n_clauses > UINT32_MAX) return ERRF(ResultSolver, ERR_PARSE, "too many clauses (%zu)", cnf->n_clauses);
    Solver *s = calloc(1, sizeof *s);
    if (!s) return ERRF(ResultSolver, ERR_NOMEM, "out of memory");
    size_t nv = cnf->n_vars, nc = cnf->n_clauses;
    size_t total = cnf->off[nc];
    s->cnf = cnf;
    s->rng = rng_new(seed);
    s->assignment = calloc(nv ? nv : 1, sizeof *s->assignment);
    s->sat = calloc(nc ? nc : 1, sizeof *s->sat);
    s->occ_off = calloc(nv + 1, sizeof *s->occ_off);
    s->occ = malloc((total ? total : 1) * sizeof *s->occ);
    s->rogue = malloc((nc ? nc : 1) * sizeof *s->rogue);
    s->rogue_pos = malloc((nc ? nc : 1) * sizeof *s->rogue_pos);
    s->stats = calloc(nv ? nv : 1, sizeof *s->stats);
    if (!s->assignment || !s->sat || !s->occ_off || !s->occ || !s->rogue || !s->rogue_pos || !s->stats) {
        solver_free(s);
        return ERRF(ResultSolver, ERR_NOMEM, "out of memory");
    }
    for (size_t ci = 0; ci < nc; ci++) {
        size_t n = cnf_clause_len(cnf, ci);
        if (n > s->max_len) s->max_len = n;
        for (size_t k = 0; k < n; k++) s->occ_off[cnf_clause(cnf, ci)[k].var + 1]++;
    }
    for (size_t v = 0; v < nv; v++) s->occ_off[v + 1] += s->occ_off[v];
    size_t *fill = malloc((nv ? nv : 1) * sizeof *fill);
    s->cands = malloc((s->max_len ? s->max_len : 1) * sizeof *s->cands);
    if (!fill || !s->cands) { free(fill); solver_free(s); return ERRF(ResultSolver, ERR_NOMEM, "out of memory"); }
    memcpy(fill, s->occ_off, nv * sizeof *fill);
    for (size_t ci = 0; ci < nc; ci++) {
        size_t n = cnf_clause_len(cnf, ci);
        for (size_t k = 0; k < n; k++) {
            Lit l = cnf_clause(cnf, ci)[k];
            s->occ[fill[l.var]++] = (Occ){ (uint32_t)ci, l.positive };
        }
    }
    free(fill);
    for (size_t v = 0; v < nv; v++) s->stats[v].rank = SIZE_MAX;
    rebuild_counts(s);
    return OK(ResultSolver, s);
}

/* Re-derive one variable's state, break and make counts from the current
 * assignment in a single pass over its occurrences. */
static void refresh_var(Solver *s, size_t v) {
    size_t satisfied = 0, rogue_occ = 0, breaks = 0, min_support = SIZE_MAX;
    bool prime = false;
    bool val = s->assignment[v];
    for (size_t i = s->occ_off[v]; i < s->occ_off[v + 1]; i++) {
        Occ o = s->occ[i];
        uint32_t c = s->sat[o.clause];
        if (c == 0) { rogue_occ++; continue; }
        if (val == o.positive) {
            satisfied++;
            if (c < min_support) min_support = c;
            if (c == 1) { prime = true; breaks++; }
        }
    }
    VarStats *st = &s->stats[v];
    st->satisfied_occurrences = satisfied;
    st->rogue_occurrences = rogue_occ;
    st->break_count = breaks;
    st->make_count = rogue_occ;  /* a rogue clause has no true literal, so every incidence is a "make" */
    st->rank = SIZE_MAX;
    if (occ_count(s, v) == 0) { st->state = VAR_UNUSED; st->min_support = 0; }
    else if (prime) { st->state = VAR_PRIME; st->min_support = 1; }
    else { st->state = VAR_NEUTRAL; st->min_support = satisfied == 0 ? 0 : min_support; }
}

static void refresh_all(Solver *s) {
    for (size_t v = 0; v < s->cnf->n_vars; v++) refresh_var(s, v);
    s->stats_dirty = false;
}

void solver_flip(Solver *s, size_t var) {
    s->assignment[var] = !s->assignment[var];
    s->stats_dirty = true;  /* states are re-derived lazily, only for candidates actually examined */
    for (size_t i = s->occ_off[var]; i < s->occ_off[var + 1]; i++) {
        Occ o = s->occ[i];
        uint32_t old = s->sat[o.clause];
        bool now_true = s->assignment[var] == o.positive;
        uint32_t nw = now_true ? old + 1 : old - 1;
        s->sat[o.clause] = nw;
        if (old == 0 && nw > 0) remove_rogue(s, o.clause);
        else if (old > 0 && nw == 0) add_rogue(s, o.clause);
    }
}

static int cand_cmp(const Candidate *a, const Candidate *b) {
    if (a->cls != b->cls) return a->cls < b->cls ? -1 : 1;                 /* neutral before prime */
    if (a->support != b->support) return a->support > b->support ? -1 : 1; /* stronger support first */
    if (a->breaks != b->breaks) return a->breaks < b->breaks ? -1 : 1;     /* fewer breaks */
    if (a->makes != b->makes) return a->makes > b->makes ? -1 : 1;         /* more makes */
    return a->var < b->var ? -1 : (a->var > b->var);
}

/* Score the variables of one rogue clause and sort them best-first. */
static size_t rank_candidates(Solver *s, size_t clause) {
    const Lit *c = cnf_clause(s->cnf, clause);
    size_t n = cnf_clause_len(s->cnf, clause);
    for (size_t k = 0; k < n; k++) {
        size_t v = c[k].var;
        refresh_var(s, v);
        const VarStats *st = &s->stats[v];
        Candidate cd = { v, 0, 0, st->break_count, st->make_count };
        switch (st->state) {
            case VAR_NEUTRAL: cd.cls = 0; cd.support = st->min_support; break;
            case VAR_PRIME:   cd.cls = 1; cd.support = 1; break;
            case VAR_UNUSED:  cd.cls = 2; cd.support = SIZE_MAX; break;
        }
        size_t j = k;  /* insertion sort: clauses are tiny */
        while (j > 0 && cand_cmp(&cd, &s->cands[j - 1]) < 0) { s->cands[j] = s->cands[j - 1]; j--; }
        s->cands[j] = cd;
    }
    for (size_t r = 0; r < n; r++) s->stats[s->cands[r].var].rank = r;
    return n;
}

static void random_assignment(Solver *s, bool majority) {
    for (size_t v = 0; v < s->cnf->n_vars; v++) {
        size_t pos = 0, total = occ_count(s, v);
        for (size_t i = s->occ_off[v]; i < s->occ_off[v + 1]; i++) pos += s->occ[i].positive;
        size_t neg = total - pos;
        s->assignment[v] = (majority && pos != neg) ? pos > neg : rng_bool(&s->rng);
    }
    rebuild_counts(s);
}

static ResultSolve make_sat(const Solver *s, size_t steps, size_t restart) {
    size_t nv = s->cnf->n_vars;
    bool *a = malloc(nv ? nv : 1);
    if (!a) return ERRF(ResultSolve, ERR_NOMEM, "out of memory");
    if (nv) memcpy(a, s->assignment, nv);
    return OK(ResultSolve, ((SolveResult){ SOLVE_SAT, a, nv, steps, restart, 0 }));
}

ResultSolve solver_solve(Solver *s, const SolverConfig *cfg) {
    const Cnf *cnf = s->cnf;
    for (size_t i = 0; i < cnf->n_clauses; i++)
        if (cnf_clause_len(cnf, i) == 0)
            return OK(ResultSolve, ((SolveResult){ SOLVE_UNKNOWN, NULL, cnf->n_vars, 0, 0, cnf->n_clauses }));
    if (cnf->n_clauses == 0) {
        memset(s->assignment, 0, cnf->n_vars);
        return make_sat(s, 0, 0);
    }
    PickFn pick = cfg->pick ? cfg->pick : pick_ranked;
    size_t total = 0, best = SIZE_MAX;
    for (size_t restart = 0; restart <= cfg->restarts; restart++) {
        random_assignment(s, restart == 0);
        for (size_t step = 0; step < cfg->max_steps; step++) {
            if (s->n_rogue == 0) return make_sat(s, total, restart);
            if (s->n_rogue < best) best = s->n_rogue;
            size_t clause = s->rogue[rng_index(&s->rng, s->n_rogue)];
            size_t n = rank_candidates(s, clause);
            if (n == 0) break;
            solver_flip(s, pick(&s->rng, s->cands, n, cfg->noise));
            total++;
            if (cfg->on_progress && cfg->progress_every && total % cfg->progress_every == 0)
                cfg->on_progress(cfg->progress_ctx, total, s->n_rogue);
        }
        if (s->n_rogue == 0) return make_sat(s, total, restart);  /* last flip solved it */
    }
    if (s->n_rogue < best) best = s->n_rogue;
    return OK(ResultSolve, ((SolveResult){ SOLVE_UNKNOWN, NULL, cnf->n_vars, total, 0, best }));
}

void solve_result_free(SolveResult *r) {
    free(r->assignment);
    r->assignment = NULL;
}

/* ---- introspection ---------------------------------------------------- */

const bool *solver_assignment(const Solver *s) { return s->assignment; }
size_t solver_unsatisfied_count(const Solver *s) { return s->n_rogue; }
bool solver_clause_is_rogue(const Solver *s, size_t c) { return s->sat[c] == 0; }
size_t solver_clause_support(const Solver *s, size_t c) { return s->sat[c]; }

const VarStats *solver_var_stats(Solver *s) {
    if (s->stats_dirty) refresh_all(s);
    return s->stats;
}

void solver_set_assignment(Solver *s, const bool *a) {
    if (s->cnf->n_vars) memcpy(s->assignment, a, s->cnf->n_vars);
    rebuild_counts(s);
}
