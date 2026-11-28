#include "sat.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>

/* Likho switch-domain defaults; the CLI -deep-dive flag overrides both. */
#define LIKHO_DOMAIN_MAX 8      /* variables in one domain, prime included */
#define LIKHO_SEARCH_NODES 64   /* backtracking budget per prime */

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

typedef struct { uint64_t hash; size_t clause; bool occupied; } TwinSlot;

static uint64_t twin_mix(uint64_t x) {
    x ^= x >> 30; x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27; x *= UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

/* Commutative signature: literal order is irrelevant. Exact comparison below
 * resolves the (very unlikely) hash collision. */
static uint64_t clause_signature(const Lit *cl, size_t n, bool invert) {
    uint64_t sum = 0, xors = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t key = ((uint64_t)cl[i].var << 1) | (uint64_t)(cl[i].positive ^ invert);
        uint64_t h = twin_mix(key + UINT64_C(0x9e3779b97f4a7c15));
        sum += h;
        xors ^= (h << (key & 31)) | (h >> ((64 - (key & 31)) & 63));
    }
    return twin_mix(sum ^ xors ^ twin_mix((uint64_t)n));
}

static bool clause_matches(const Lit *a, size_t na, const Lit *b, size_t nb, bool async) {
    if (na != nb) return false;
    for (size_t i = 0; i < na; i++) {
        size_t ac = 0, bc = 0;
        for (size_t j = 0; j < na; j++) {
            ac += a[j].var == a[i].var && a[j].positive == a[i].positive;
            bc += b[j].var == a[i].var && b[j].positive == (async ? !a[i].positive : a[i].positive);
        }
        if (ac != bc) return false;
    }
    return true;
}

static bool twin_table_contains(const TwinSlot *table, size_t cap, uint64_t hash,
                                const Lit *cl, size_t n, const Cnf *kept, bool async) {
    size_t slot = (size_t)hash & (cap - 1);
    while (table[slot].occupied) {
        if (table[slot].hash == hash) {
            size_t ci = table[slot].clause;
            if (clause_matches(cl, n, cnf_clause(kept, ci), cnf_clause_len(kept, ci), async))
                return true;
        }
        slot = (slot + 1) & (cap - 1);
    }
    return false;
}

static void twin_table_insert(TwinSlot *table, size_t cap, uint64_t hash, size_t clause) {
    size_t slot = (size_t)hash & (cap - 1);
    while (table[slot].occupied) slot = (slot + 1) & (cap - 1);
    table[slot] = (TwinSlot){ .hash = hash, .clause = clause, .occupied = true };
}

bool no_twins(const Cnf *src, Cnf *out) {
    if (!src || !out || !src->off) return false;
    *out = (Cnf){0};
    size_t max_lits = src->off[src->n_clauses];
    if (src->n_clauses > (SIZE_MAX - 1) / 2) return false;
    size_t need = src->n_clauses * 2 + 1;
    size_t cap = 8;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) return false;
        cap *= 2;
    }
    TwinSlot *sync = calloc(cap, sizeof *sync);
    TwinSlot *async = calloc(cap, sizeof *async);
    out->off = calloc(src->n_clauses + 1, sizeof *out->off);
    out->lits = malloc((max_lits ? max_lits : 1) * sizeof *out->lits);
    if (!sync || !async || !out->off || !out->lits) {
        free(sync); free(async); cnf_free(out); return false;
    }
    out->n_vars = src->n_vars;
    for (size_t i = 0; i < src->n_clauses; i++) {
        const Lit *cl = cnf_clause(src, i);
        size_t n = cnf_clause_len(src, i);
        uint64_t hs = clause_signature(cl, n, false);
        bool duplicate = twin_table_contains(sync, cap, hs, cl, n, out, false) ||
                         twin_table_contains(async, cap, hs, cl, n, out, true);
        if (!duplicate) {
            size_t kept = out->n_clauses;
            if (n) memcpy(out->lits + out->off[kept], cl, n * sizeof *out->lits);
            out->n_clauses++;
            out->off[out->n_clauses] = out->off[kept] + n;
            twin_table_insert(sync, cap, hs, kept);
            twin_table_insert(async, cap, clause_signature(cl, n, true), kept);
        } else {
            out->off[out->n_clauses + 1] = out->off[out->n_clauses];
        }
    }
    free(sync); free(async);
    return true;
}

bool cnf_negate(const Cnf *src, Cnf *out) {
    if (!src || !out || !src->off) return false;
    *out = (Cnf){0};
    size_t max_lits = src->off[src->n_clauses];
    out->off = malloc((src->n_clauses + 1) * sizeof *out->off);
    out->lits = malloc((max_lits ? max_lits : 1) * sizeof *out->lits);
    if (!out->off || !out->lits) { cnf_free(out); return false; }
    out->n_vars = src->n_vars;
    out->n_clauses = src->n_clauses;
    memcpy(out->off, src->off, (src->n_clauses + 1) * sizeof *out->off);
    for (size_t i = 0; i < max_lits; i++) {
        out->lits[i].var = src->lits[i].var;
        out->lits[i].positive = !src->lits[i].positive;
    }
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
        if (line[0] == '%') break;   /* DIMACS end-of-file marker; trailing tokens are ignored */

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
        .likho_domain_max = LIKHO_DOMAIN_MAX, .likho_search_nodes = LIKHO_SEARCH_NODES,
        .likho_domains_first = false, .likho_pre_cook = INIT_ZERO,
        .terminator_rounds = 16, .terminator_order = TERM_FORWARD,
        .neighborhood_timeout_ms = 1000,
        .neighborhood_start = NB_MOST,
        .baddymove_allow = 0.0,
        .gob_recult_play = 0, .gob_recult_set = 0.0, .gob_recult_has_set = false,
        .gob_recult_coef_k = 2,
        .yanus_attempts = 0,
        .yanus_timeout_ms = 30,
        .yanus_saturate = 0.0,
        .yanus_glowest = true,
        .yanus_terminator = true,
        .yanus_baddymove = false,
        .yanus_diving = 0.0,
        .knots_random_mode = false,
        .knots_seed = 1,
        .knots_rounds = 1,
        .knots_sort = KNOTS_SORT_NONE,
        .knots_glowest = false,
        .knots_terminator = false,
        .knots_likho = false,
        .knots_yanus = false,
        .knots_gob_recult = false,
        .knots_mono = false,
        .knots_reverse = false,
        .knots_keep_bad = 0,
        .north_max_dive = 8,
        .path_rider = 0,
        .m_tables = 1,
        .timeout_n_rerun_ms = 0,
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
const Cnf *solver_cnf(const Solver *s) { return s->cnf; }

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
 * assignment in a single pass over its occurrences.
 *
 * Ranking invariant (relied on by the group merge):
 *   VAR_PRIME   <=> break_count >= 1   (sole support of at least one solved clause)
 *   VAR_NEUTRAL <=> break_count == 0   (flipping can never un-solve a clause)
 * Neutrality is a property of the *current* assignment, not of the variable. */
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

typedef struct { size_t key, den, var; } Rare;

/* Ascending key/den, ties by variable id. den is 1 for the frequency orders and
 * the variable's occurrence count for the overlap order; ratios are compared by
 * cross-multiplication so the order stays exact. A zero denominator (variable
 * with no occurrences) sorts last. */
static bool rare_before(Rare a, Rare b) {
    if ((a.den == 0) != (b.den == 0)) return b.den == 0;
    unsigned __int128 l = (unsigned __int128)a.key * b.den;
    unsigned __int128 r = (unsigned __int128)b.key * a.den;
    if (l != r) return l < r;
    return a.var < b.var;
}

static void rare_push(Rare *h, size_t *n, Rare x) {
    size_t i = (*n)++;
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (!rare_before(x, h[p])) break;
        h[i] = h[p];
        i = p;
    }
    h[i] = x;
}

static void rare_sift_down(Rare *h, size_t n, size_t i, Rare x) {
    for (;;) {
        size_t l = 2 * i + 1;
        if (l >= n) break;
        size_t c = l;
        if (l + 1 < n && rare_before(h[l + 1], h[l])) c = l + 1;
        if (!rare_before(h[c], x)) break;
        h[i] = h[c];
        i = c;
    }
    h[i] = x;
}

static Rare rare_pop(Rare *h, size_t *n) {
    Rare top = h[0], last = h[--(*n)];
    if (*n) rare_sift_down(h, *n, 0, last);
    return top;
}

/* Rarest = fewest clauses holding x or !x, whichever is larger; glowest = most.
 * Only clauses that are still unsatisfied count, so satisfying a clause removes
 * the overlapping occurrences of every variable it contains. */
static size_t rare_key(const uint32_t *pos, const uint32_t *neg, size_t v, bool most) {
    size_t m = pos[v] > neg[v] ? pos[v] : neg[v];
    return most ? SIZE_MAX - m : m;
}

typedef enum { ORDER_RAREST, ORDER_GLOWEST, ORDER_MIN_OVERLAP } InitOrder;

/* Current key and denominator of v. min-overlap ranks a variable by the share of
 * its occurrences that sit in already satisfied clauses. */
static void order_key(InitOrder mode, const uint32_t *pos, const uint32_t *neg,
                      const uint32_t *overlap, const size_t *total,
                      size_t v, size_t *key, size_t *den) {
    if (mode == ORDER_MIN_OVERLAP) { *key = overlap[v]; *den = total[v]; }
    else { *key = rare_key(pos, neg, v, mode == ORDER_GLOWEST); *den = 1; }
}

static void order_push(Rare *h, size_t *hn, InitOrder mode, const uint32_t *pos,
                       const uint32_t *neg, const uint32_t *overlap,
                       const size_t *total, size_t v) {
    size_t key, den;
    order_key(mode, pos, neg, overlap, total, v, &key, &den);
    rare_push(h, hn, (Rare){ key, den, v });
}

/* Constructive start. rarest/glowest take at every step the unset variable with
 * the fewest/most open clauses holding x or !x (the larger polarity count, ties
 * by id). min-overlap takes the most frequent variable first (glowest's pick,
 * ties by id), then the one with the smallest share of its occurrences already
 * in satisfied clauses, ties by id. A key only moves one way, so the current
 * entry is re-pushed whenever it changes and a stale entry is discarded when it
 * pops. */
static bool init_order(Solver *s, InitOrder mode) {
    const Cnf *cnf = s->cnf;
    size_t nv = cnf->n_vars, nc = cnf->n_clauses;
    uint32_t *unset = malloc((nc ? nc : 1) * sizeof *unset);   /* unassigned variables left in each clause */
    unsigned char *done = calloc(nc ? nc : 1, 1);              /* clause already has a true literal */
    uint32_t *pos = calloc(nv ? nv : 1, sizeof *pos);          /* polarity counts of open clauses (frequency orders) */
    uint32_t *neg = calloc(nv ? nv : 1, sizeof *neg);
    uint32_t *overlap = calloc(nv ? nv : 1, sizeof *overlap);  /* satisfied occurrences, min-overlap order */
    size_t *total = calloc(nv ? nv : 1, sizeof *total);        /* all occurrences, min-overlap denominator */
    unsigned char *assigned = calloc(nv ? nv : 1, 1);
    Rare *heap = malloc((nv + s->occ_off[nv] + 1) * sizeof *heap);
    if (!unset || !done || !pos || !neg || !overlap || !total || !assigned || !heap) {
        free(unset); free(done); free(pos); free(neg); free(overlap); free(total);
        free(assigned); free(heap);
        return false;
    }
    for (size_t c = 0; c < nc; c++) unset[c] = (uint32_t)cnf_clause_len(cnf, c);
    size_t hn = 0;
    for (size_t v = 0; v < nv; v++) {
        total[v] = occ_count(s, v);
        for (size_t k = s->occ_off[v]; k < s->occ_off[v + 1]; k++)
            if (s->occ[k].positive) pos[v]++; else neg[v]++;
    }
    /* min-overlap's first pick is glowest's, so its heap starts after step 0. */
    if (mode != ORDER_MIN_OVERLAP)
        for (size_t v = 0; v < nv; v++)
            order_push(heap, &hn, mode, pos, neg, overlap, total, v);

    for (size_t step = 0; step < nv; step++) {
        size_t v;
        if (mode == ORDER_MIN_OVERLAP && step == 0) {
            v = 0;
            for (size_t u = 1; u < nv; u++) {
                size_t m = pos[u] > neg[u] ? pos[u] : neg[u];
                size_t bm = pos[v] > neg[v] ? pos[v] : neg[v];
                if (m > bm) v = u;
            }
        } else {
            Rare top;
            size_t key, den;
            do {
                top = rare_pop(heap, &hn);
                order_key(mode, pos, neg, overlap, total, top.var, &key, &den);
            } while (assigned[top.var] || top.key != key || top.den != den);
            v = top.var;
        }
        size_t fp = 0, fn = 0, op = 0, on = 0, tp = 0, tn = 0;
        for (size_t k = s->occ_off[v]; k < s->occ_off[v + 1]; k++) {
            Occ o = s->occ[k];
            if (o.positive) tp++; else tn++;
            if (done[o.clause]) continue;
            if (unset[o.clause] == 1) { if (o.positive) fp++; else fn++; }
            else                      { if (o.positive) op++; else on++; }
        }
        bool val;
        if (fp != fn) val = fp > fn;
        else if (op != on) val = op > on;
        else if (tp != tn) val = tp > tn;
        else val = false;
        s->assignment[v] = val;
        assigned[v] = 1;
        for (size_t k = s->occ_off[v]; k < s->occ_off[v + 1]; k++) {
            Occ o = s->occ[k];
            unset[o.clause]--;
            if (done[o.clause] || o.positive != val) continue;
            done[o.clause] = 1;
            const Lit *cl = cnf_clause(cnf, o.clause);
            size_t len = cnf_clause_len(cnf, o.clause);
            for (size_t j = 0; j < len; j++) {
                size_t u = cl[j].var;
                if (assigned[u]) continue;
                if (mode == ORDER_MIN_OVERLAP) overlap[u]++;
                else if (cl[j].positive) pos[u]--; else neg[u]--;
                if (mode == ORDER_MIN_OVERLAP && step == 0) continue;   /* heap is built after this step */
                order_push(heap, &hn, mode, pos, neg, overlap, total, u);
            }
        }
        if (mode == ORDER_MIN_OVERLAP && step == 0) {
            hn = 0;   /* every unset variable gets an overlap entry, not a glowest one */
            for (size_t u = 0; u < nv; u++)
                if (!assigned[u]) order_push(heap, &hn, mode, pos, neg, overlap, total, u);
        }
    }
    free(unset); free(done); free(pos); free(neg); free(overlap); free(total);
    free(assigned); free(heap);
    rebuild_counts(s);
    return true;
}

bool solver_init_rarest(Solver *s) { return init_order(s, ORDER_RAREST); }
bool solver_init_glowest(Solver *s) { return init_order(s, ORDER_GLOWEST); }
bool solver_init_min_overlap(Solver *s) { return init_order(s, ORDER_MIN_OVERLAP); }

static bool nb_timed_out(struct timespec deadline, bool has_deadline) {
    if (!has_deadline) return false;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec > deadline.tv_sec ||
           (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec);
}

/* Yanus replaces original variables by fresh pairs in the signed-equation
 * sense: v -> (a + b), so +v becomes +a,+b and -v becomes -a,-b. No clauses
 * are added; the mapped value of v is a XOR b. */
bool solver_init_yanus(Solver *s, const SolverConfig *cfg) {
    const Cnf *cnf = s->cnf;
    size_t nv = cnf->n_vars, nc = cnf->n_clauses;
    size_t attempts = cfg ? cfg->yanus_attempts : 0;
    size_t timeout_ms = cfg ? cfg->yanus_timeout_ms : 30;
    bool use_glowest = cfg ? cfg->yanus_glowest : true;
    bool use_term = cfg ? cfg->yanus_terminator : true;
    bool use_bad = cfg ? cfg->yanus_baddymove : false;
    if (!nv) return solver_init_glowest(s);
    if (attempts == 0 && timeout_ms == 0) return solver_init_glowest(s);
    if (!use_glowest && !use_term && !use_bad) return solver_init_glowest(s);

    bool *best = malloc(nv * sizeof *best);
    size_t *rep0 = malloc(nv * sizeof *rep0), *rep1 = malloc(nv * sizeof *rep1);
    bool *mapped = malloc(nv * sizeof *mapped);
    if (!best || !rep0 || !rep1 || !mapped) {
        free(best); free(rep0); free(rep1); free(mapped); return false;
    }
    for (size_t v = 0; v < nv; v++) { rep0[v] = SIZE_MAX; rep1[v] = SIZE_MAX; }

    size_t best_rogue = SIZE_MAX;
    size_t n_replaced = 0, ex_vars = nv;
    bool found = false, fail = false;
    double saturate = cfg ? cfg->yanus_saturate : solver_config_default().yanus_saturate;
    double diving = cfg ? cfg->yanus_diving : 0.0;
    size_t nnv = 0; bool nnv_set = false;

    struct timespec deadline = {0, 0};
    bool has_deadline = timeout_ms > 0;
    if (has_deadline) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += (time_t)(timeout_ms / 1000);
        deadline.tv_nsec += (long)((timeout_ms % 1000) * 1000000UL);
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    }

    for (size_t attempt = 0; (attempts == 0 || attempt < attempts) && !fail && best_rogue > 0; attempt++) {
        if (has_deadline && nb_timed_out(deadline, true)) break;
        size_t d = 1 + rng_index(&s->rng, nv);
        if (saturate > 0.0) {
            double want = saturate * (double)nc;
            if (want < 1.0) d = 1;
            else if (want >= (double)SIZE_MAX) d = SIZE_MAX;
            else d = (size_t)want;
        }
        size_t add = d;
        if (diving > 0.0) {
            double next = nnv_set ? (double)nnv * diving : (double)d * diving;
            if (next < 1.0) break; /* nnv == 0 -> timeout */
            nnv = next >= (double)SIZE_MAX ? SIZE_MAX : (size_t)next;
            nnv_set = true;
            add = nnv;
        } else {
            /* non-diving: fresh replacements each attempt */
            for (size_t v = 0; v < nv; v++) { rep0[v] = SIZE_MAX; rep1[v] = SIZE_MAX; }
            n_replaced = 0;
            ex_vars = nv;
        }

        size_t pairs = add / 2, singles = add % 2;
        for (size_t k = 0; k < pairs && n_replaced < nv; k++) {
            size_t v = SIZE_MAX;
            for (size_t tries = 0; tries < 1024 && v == SIZE_MAX; tries++) {
                size_t c = rng_index(&s->rng, nv);
                if (rep0[c] == SIZE_MAX) v = c;
            }
            if (v == SIZE_MAX) { for (size_t c = 0; c < nv; c++) if (rep0[c] == SIZE_MAX) { v = c; break; } }
            if (v == SIZE_MAX) { fail = true; break; }
            if (ex_vars > UINT32_MAX - 1) { fail = true; break; }
            rep0[v] = ex_vars++;
            rep1[v] = ex_vars++;
            n_replaced++;
        }
        if (singles && n_replaced < nv && ex_vars <= UINT32_MAX) {
            size_t v = SIZE_MAX;
            for (size_t tries = 0; tries < 1024 && v == SIZE_MAX; tries++) {
                size_t c = rng_index(&s->rng, nv);
                if (rep0[c] == SIZE_MAX) v = c;
            }
            if (v == SIZE_MAX) { for (size_t c = 0; c < nv; c++) if (rep0[c] == SIZE_MAX) { v = c; break; } }
            if (v != SIZE_MAX) { rep0[v] = ex_vars++; n_replaced++; }
        }

        size_t total_lits = 0;
        for (size_t ci = 0; ci < nc; ci++) {
            size_t len = cnf_clause_len(cnf, ci);
            const Lit *cl = cnf_clause(cnf, ci);
            for (size_t k = 0; k < len; k++) total_lits += (rep0[cl[k].var] != SIZE_MAX) + (rep1[cl[k].var] != SIZE_MAX);
        }
        Cnf ex = { .n_vars = ex_vars, .n_clauses = nc,
                   .off = malloc((nc + 1) * sizeof *ex.off),
                   .lits = malloc((total_lits ? total_lits : 1) * sizeof *ex.lits) };
        if (!ex.off || !ex.lits) { cnf_free(&ex); fail = true; break; }
        size_t pos = 0;
        for (size_t ci = 0; ci < nc; ci++) {
            ex.off[ci] = pos;
            size_t len = cnf_clause_len(cnf, ci);
            const Lit *cl = cnf_clause(cnf, ci);
            for (size_t k = 0; k < len; k++) {
                size_t v = cl[k].var;
                bool sign = cl[k].positive;
                if (rep0[v] != SIZE_MAX) ex.lits[pos++] = (Lit){ (uint32_t)rep0[v], sign };
                if (rep1[v] != SIZE_MAX) ex.lits[pos++] = (Lit){ (uint32_t)rep1[v], sign };
            }
        }
        ex.off[nc] = pos;

        ResultSolver rs = solver_new(&ex, rng_next_u64(&s->rng));
        if (!rs.ok) { cnf_free(&ex); fail = true; break; }
        if (use_glowest) {
            if (!solver_init_glowest(rs.val)) { solver_free(rs.val); cnf_free(&ex); fail = true; break; }
        } else {
            for (size_t v = 0; v < ex.n_vars; v++) rs.val->assignment[v] = rng_bool(&rs.val->rng);
            rebuild_counts(rs.val);
        }
        const bool *ea = solver_assignment(rs.val);
        for (size_t v = 0; v < nv; v++) {
            if (rep1[v] != SIZE_MAX) mapped[v] = ea[rep0[v]] ^ ea[rep1[v]];
            else if (rep0[v] != SIZE_MAX) mapped[v] = ea[rep0[v]];
            else mapped[v] = ea[v];
        }
        solver_free(rs.val);
        cnf_free(&ex);
        solver_set_assignment(s, mapped);
        if (use_bad)
            solver_baddymove(s, cfg ? cfg->terminator_rounds : 16,
                             cfg ? cfg->terminator_order : TERM_FORWARD,
                             cfg ? cfg->baddymove_allow : 0.0);
        else if (use_term)
            solver_terminator(s, cfg ? cfg->terminator_rounds : 16,
                              cfg ? cfg->terminator_order : TERM_FORWARD);
        if (!found || s->n_rogue < best_rogue) {
            best_rogue = s->n_rogue;
            memcpy(best, s->assignment, nv);
            found = true;
        }
    }
    free(rep0); free(rep1); free(mapped);
    if (fail) { free(best); return false; }
    if (found) solver_set_assignment(s, best);
    else if (!solver_init_glowest(s)) { free(best); return false; }
    free(best);
    return true;
}

bool solver_init_reverse(Solver *s, InitMode init) {
    Cnf neg = {0};
    if (!cnf_negate(s->cnf, &neg)) return false;
    ResultSolver rs = solver_new(&neg, 0x9e3779b97f4a7c15ULL);
    if (!rs.ok) { cnf_free(&neg); return false; }
    Solver *t = rs.val;
    bool ok = false;
    if (init == INIT_RAREST) ok = solver_init_rarest(t);
    else if (init == INIT_GLOWEST) ok = solver_init_glowest(t);
    else if (init == INIT_MIN_OVERLAP) ok = solver_init_min_overlap(t);
    else { solver_free(t); cnf_free(&neg); return false; }
    if (ok) {
        size_t nv = s->cnf->n_vars;
        bool *a = malloc((nv ? nv : 1) * sizeof *a);
        if (!a) ok = false;
        else {
            for (size_t i = 0; i < nv; i++) a[i] = !t->assignment[i];
            solver_set_assignment(s, a);
            free(a);
        }
    }
    solver_free(t);
    cnf_free(&neg);
    return ok;
}

/* ---- Gob: linear relaxation of the clause system (full or square) ------ */

/* Every clause becomes the signed equation sum(+-x) = 1: a positive literal is
 * +x, a negative one -x. Cyclic Kaczmarz projections (one per clause per sweep)
 * solve it in the least-squares sense. A greedy pass then flips a decoded
 * variable whenever it satisfies more rogue clauses than it breaks, adapting the
 * bits to the formula. Deterministic, no RNG.
 *
 * Two decodes share that system. `gob` shifts each negative value by the
 * smallest non-negative amount (so -x = 1 decodes to false, not true), rounds
 * and takes mod 2. `gob-sign` takes the sign instead: the equation asks for two
 * true literals per clause, which a satisfying assignment usually does not give,
 * so the Kaczmarz point is not near SAT and the magnitude is not a SAT signal,
 * but the sign still correlates with a solution. See the sign-decode test. */
#define GOB_SWEEPS 64     /* Kaczmarz passes over the whole clause system */
#define GOB_SQUARE_SWEEPS 512  /* the square system converges slower than the full one */
#define GOB_PASSES 4      /* greedy adaptation passes over the decoded bits */

/* One exact Kaczmarz projection of the real vector onto clause ci's equation. */
static void gob_project(const Cnf *cnf, double *v, size_t ci) {
    const Lit *cl = cnf_clause(cnf, ci);
    size_t len = cnf_clause_len(cnf, ci);
    if (len == 0) return;
    double sum = 0;
    for (size_t k = 0; k < len; k++)
        sum += cl[k].positive ? v[cl[k].var] : -v[cl[k].var];
    double step = (1.0 - sum) / (double)len;
    for (size_t k = 0; k < len; k++)
        v[cl[k].var] += cl[k].positive ? step : -step;
}

/* Magnitude decode: shift the negative away, round to nearest, take mod 2. */
static void gob_decode_parity(const double *v, bool *a, size_t nv) {
    for (size_t i = 0; i < nv; i++) {
        double u = v[i];
        if (u < 0) {
            long long sh = (long long)(-u);
            if ((double)sh < -u) sh++;
            u += (double)sh;
        }
        a[i] = (((long long)(u + 0.5)) & 1) != 0;
    }
}

/* Sign decode: positive is true, zero or negative is false. */
static void gob_decode_sign(const double *v, bool *a, size_t nv) {
    for (size_t i = 0; i < nv; i++) a[i] = v[i] > 0;
}

/* Step 4: flip a decoded variable when it satisfies more rogue clauses than it
 * breaks. */
static void gob_repair(Solver *s) {
    size_t nv = s->cnf->n_vars;
    for (int pass = 0; pass < GOB_PASSES; pass++) {
        size_t flips = 0;
        for (size_t i = 0; i < nv; i++) {
            refresh_var(s, i);
            const VarStats *st = &s->stats[i];
            if (st->make_count > st->break_count) { solver_flip(s, i); flips++; }
        }
        if (flips == 0) break;
    }
}

/* Full-system start: project the filtered clauses, decode, repair. */
static bool gob_full(Solver *s, bool sign) {
    Cnf filtered = {0};
    if (!no_twins(s->cnf, &filtered)) return false;
    size_t nv = s->cnf->n_vars, nc = filtered.n_clauses;
    double *v = calloc(nv ? nv : 1, sizeof *v);
    bool *a = malloc((nv ? nv : 1) * sizeof *a);
    if (!v || !a) { free(v); free(a); cnf_free(&filtered); return false; }
    for (size_t sweep = 0; sweep < GOB_SWEEPS; sweep++)
        for (size_t ci = 0; ci < nc; ci++) gob_project(&filtered, v, ci);
    if (sign) gob_decode_sign(v, a, nv);
    else gob_decode_parity(v, a, nv);
    solver_set_assignment(s, a);
    gob_repair(s);
    free(v); free(a); cnf_free(&filtered);
    return true;
}

bool solver_init_gob(Solver *s) { return gob_full(s, false); }
bool solver_init_gob_sign(Solver *s) { return gob_full(s, true); }

bool solver_init_gob_square(Solver *s, GobSquareCheck *check) {
    Cnf filtered = {0};
    if (!no_twins(s->cnf, &filtered)) return false;
    size_t nv = s->cnf->n_vars, nc = filtered.n_clauses;
    double *v = calloc(nv ? nv : 1, sizeof *v);
    bool *a = malloc((nv ? nv : 1) * sizeof *a);
    bool *sel = calloc(nc ? nc : 1, 1);
    if (!v || !a || !sel) { free(v); free(a); free(sel); cnf_free(&filtered); return false; }
    size_t neq = nv < nc ? nv : nc;                 /* phase 1: n of the m filtered equations */
    if (neq) {
        for (size_t ei = 0; ei < neq; ei++) sel[ei * nc / neq] = true;
        for (size_t sweep = 0; sweep < GOB_SQUARE_SWEEPS; sweep++)      /* phase 2 */
            for (size_t ci = 0; ci < nc; ci++)
                if (sel[ci]) gob_project(&filtered, v, ci);
    }
    if (check) {                                    /* phase 3: the rest of the filtered system */
        check->selected_bad = check->other_bad = check->other_total = 0;
        for (size_t ci = 0; ci < nc; ci++) {
            const Lit *cl = cnf_clause(&filtered, ci);
            size_t len = cnf_clause_len(&filtered, ci);
            double sum = 0;
            for (size_t k = 0; k < len; k++)
                sum += cl[k].positive ? v[cl[k].var] : -v[cl[k].var];
            bool bad = len && (sum < 1.0 - 1e-9 || sum > 1.0 + 1e-9);
            if (sel[ci]) { if (bad) check->selected_bad++; }
            else { check->other_total++; if (bad) check->other_bad++; }
        }
    }
    gob_decode_sign(v, a, nv);
    solver_set_assignment(s, a);
    gob_repair(s);
    free(v); free(a); free(sel); cnf_free(&filtered);
    return true;
}


static void gob_recult_kh(size_t nc, Rng *rng, size_t *k_out, size_t *h_out) {
    size_t k = 2;
    if (nc > 0) {
        k = (size_t)rng_next_u64(rng) % nc;
        if (k == 0) k = 1 % nc;
        if (k == 0) k = 1;
        if (k % 2 == 1) k = (k + 1 <= nc ? k + 1 : k - 1);
        if (k == 0) k = 2;
    }
    *k_out = k; *h_out = k / 2;
}

/* M Y = b - 1 and 1ᵀ M⁻¹ = ((-1)^i), so the coupling sum implied by b is
 * B = Σ_i (-1)^i (b_i - 1). b_i is the number of true literals in clause i. */
static double gob_recult_b_center(const Cnf *filtered, const bool *a) {
    double center = 0.0;
    for (size_t ci = 0; ci < filtered->n_clauses; ci++) {
        const Lit *cl = cnf_clause(filtered, ci);
        size_t b = 0;
        for (size_t t = 0; t < cnf_clause_len(filtered, ci); t++)
            if (cl[t].positive ? a[cl[t].var] : !a[cl[t].var]) b++;
        center += (ci % 2 == 0 ? 1.0 : -1.0) * ((double)b - 1.0);
    }
    return center;
}

static void gob_recult_run(const Cnf *filtered, size_t nv, size_t nc, size_t k, size_t h,
                           double baseline, double *x, double *y) {
    size_t nx = nv ? nv : 1, ny = nc ? nc : 1;
    for (size_t i = 0; i < nx; i++) x[i] = 0.0;
    for (size_t i = 0; i < ny; i++) y[i] = 0.0;
    for (size_t sweep = 0; sweep < GOB_SWEEPS; sweep++) {
        for (size_t ci = 0; ci < nc; ci++) {
            const Lit *cl = cnf_clause(filtered, ci);
            size_t len = cnf_clause_len(filtered, ci);
            size_t j1 = nc ? (ci + k) % nc : 0;
            size_t j2 = nc ? (ci + h) % nc : 0;
            double s1 = ((ci + k) % 2 == 0) ? 1.0 : -1.0;
            double sum = 0;
            for (size_t t = 0; t < len; t++)
                sum += cl[t].positive ? x[cl[t].var] : -x[cl[t].var];
            double rhs = 1.0 + y[ci] + s1 * y[j1] + y[j2];
            double step = (sum - rhs) / ((double)len + 3.0);
            if (step == 0.0) continue;
            for (size_t t = 0; t < len; t++)
                x[cl[t].var] -= step * (cl[t].positive ? 1.0 : -1.0);
            y[ci] -= step * (-1.0); y[j1] -= step * (-s1); y[j2] -= step * (-1.0);
        }
        if (nc > 0) {
            double sumy = 0; for (size_t i = 0; i < nc; i++) sumy += y[i];
            double step = (sumy - baseline) / (double)nc;
            if (step != 0.0) for (size_t i = 0; i < nc; i++) y[i] -= step;
        }
    }
}

bool solver_init_gob_recult(Solver *s, const SolverConfig *cfg) {
    const Cnf *cnf = s->cnf;
    size_t nv = cnf->n_vars;
    Cnf filtered = {0};
    if (!no_twins(cnf, &filtered)) return false;
    size_t nc = filtered.n_clauses;
    size_t nx = nv ? nv : 1, ny = nc ? nc : 1;
    double *x = calloc(nx, sizeof *x), *y = calloc(ny, sizeof *y);
    bool *a = malloc(nx), *best_a = malloc(nx);
    if (!x || !y || !a || !best_a) { free(x); free(y); free(a); free(best_a); cnf_free(&filtered); return false; }

    size_t k, h;
    gob_recult_kh(nc, &s->rng, &k, &h);

    bool auto_baseline = !cfg || (!cfg->gob_recult_has_set && cfg->gob_recult_play == 0);
    if (!auto_baseline) {
        double baseline = cfg->gob_recult_has_set ? cfg->gob_recult_set
                          : (double)(rng_next_u64(&s->rng) % cfg->gob_recult_play);
        gob_recult_run(&filtered, nv, nc, k, h, baseline, x, y);
        gob_decode_sign(x, a, nv);
        solver_set_assignment(s, a); gob_repair(s);
        free(x); free(y); free(a); free(best_a); cnf_free(&filtered); return true;
    }

    /* Center the baseline search on the value implied by BaddyMove's b vector. */
    SolverConfig bcfg = cfg ? *cfg : solver_config_default();
    bcfg.init = INIT_BADDYMOVE; bcfg.likho_pre_cook = INIT_NEIGHBORHOOD0;
    bcfg.max_steps = 0; bcfg.restarts = 0; bcfg.terminator_rounds = 32;
    bcfg.terminator_order = TERM_RANDOM; bcfg.baddymove_allow = 0.1;
    bcfg.neighborhood_timeout_ms = 1000;
    double b_center = 0.0;
    ResultSolver brs = solver_new(cnf, cfg && cfg->seed ? cfg->seed : 0x9e3779b97f4a7c15ULL);
    if (brs.ok) {
        if (solver_init_baddymove(brs.val, &bcfg))
            b_center = gob_recult_b_center(&filtered, solver_assignment(brs.val));
        solver_free(brs.val);
    }
    long long center = (long long)(b_center >= 0 ? b_center + 0.5 : b_center - 0.5);

    size_t coef_k = cfg ? cfg->gob_recult_coef_k : solver_config_default().gob_recult_coef_k;
    size_t span = nc ? nc / 4 + 1 : 1;
    /* shortcut: span capped at 128, widen if large instances need a bigger window */
    if (span > 128) span = 128;
    uint64_t radius_u = (uint64_t)coef_k * (uint64_t)span;
    if (radius_u > (uint64_t)LLONG_MAX) radius_u = (uint64_t)LLONG_MAX;
    long long radius = (long long)radius_u;

    size_t best_unsat = SIZE_MAX; bool found = false;
    for (long long d = 0; d <= radius && best_unsat > 0; d++) {
        long long cands[3] = { center, center - d, center + d };
        int ntry = (d == 0) ? 1 : 3;
        for (int t = 0; t < ntry; t++) {
            gob_recult_run(&filtered, nv, nc, k, h, (double)cands[t], x, y);
            gob_decode_sign(x, a, nv);
            solver_set_assignment(s, a); gob_repair(s);
            if (!found || s->n_rogue < best_unsat) {
                best_unsat = s->n_rogue;
                memcpy(best_a, s->assignment, nv * sizeof *best_a);
                found = true;
                if (best_unsat == 0) break;
            }
        }
    }
    if (found) { solver_set_assignment(s, best_a); gob_repair(s); }
    else { gob_recult_run(&filtered, nv, nc, k, h, (double)center, x, y); gob_decode_sign(x, a, nv); solver_set_assignment(s, a); gob_repair(s); }
    free(x); free(y); free(a); free(best_a); cnf_free(&filtered);
    return true;
}

/* ---- Likho: zero start + switch-domain repair pass --------------------- */

/* A switch domain of a prime is the set of unflipped variables that must flip
 * with it so that no currently satisfied clause becomes rogue. A neutral is
 * its own domain; a prime needs a repair for every clause it solely supports,
 * and a repair may itself be a prime, so domains can chain. Domains are
 * recomputed after each firing and `flipped` enforces the gold rule (every
 * variable fires once): two primes whose domains share a variable share that
 * single flip, and a prime whose domain conflicts with an already fired one is
 * simply not fired. */
/* shortcut: a prime whose domain needs more than either bound is skipped, not
 * solved; -deep-dive raises the bounds. */

typedef struct {
    Solver *s;
    const unsigned char *flipped;
    unsigned char *in;   /* membership of the domain under construction */
    size_t *dom;         /* dom[0] is the prime the domain is for */
    size_t n, nodes, max_vars, max_nodes;
} LikhoSearch;

/* Support of `clause` after flipping every variable of the domain once.
 * Clauses with no domain variable keep their count and are never examined. */
static uint32_t likho_sat_after(const LikhoSearch *d, size_t clause) {
    const Lit *c = cnf_clause(d->s->cnf, clause);
    size_t n = cnf_clause_len(d->s->cnf, clause);
    uint32_t cnt = 0;
    for (size_t k = 0; k < n; k++)
        cnt += lit_value(c[k], d->s->assignment[c[k].var]) != d->in[c[k].var];
    return cnt;
}

/* true when flipping v alone cannot un-satisfy a clause; those "free" repairs
 * are tried first because they add no new breakage to the domain. */
static bool likho_breaks_nothing(const Solver *s, size_t v) {
    bool val = s->assignment[v];
    for (size_t i = s->occ_off[v]; i < s->occ_off[v + 1]; i++) {
        Occ o = s->occ[i];
        if (o.positive == val && s->sat[o.clause] == 1) return false;
    }
    return true;
}

/* Close the domain: every clause it broke needs a repair from one of its own
 * false literals, recursively. A clause with no free false literal backtracks. */
static bool likho_close(LikhoSearch *d) {
    if (++d->nodes > d->max_nodes) return false;
    Solver *s = d->s;
    for (size_t i = 0; i < d->n; i++) {
        size_t v = d->dom[i];
        for (size_t j = s->occ_off[v]; j < s->occ_off[v + 1]; j++) {
            size_t c = s->occ[j].clause;
            if (likho_sat_after(d, c) != 0) continue;
            if (d->n >= d->max_vars) return false;
            const Lit *cl = cnf_clause(s->cnf, c);
            size_t len = cnf_clause_len(s->cnf, c);
            for (int pass = 0; pass < 2; pass++)
                for (size_t k = 0; k < len; k++) {
                    size_t u = cl[k].var;
                    if (d->in[u] || d->flipped[u]) continue;
                    if (lit_value(cl[k], s->assignment[u])) continue;   /* must be false to help */
                    if ((pass == 0) != likho_breaks_nothing(s, u)) continue;
                    d->in[u] = 1; d->dom[d->n++] = u;
                    if (likho_close(d)) return true;
                    d->n--; d->in[u] = 0;
                }
            return false;   /* this clause cannot be repaired: backtrack */
        }
    }
    return true;   /* nothing broke */
}

/* Switch domain of v, or 0 when none fits the size/node bounds. On success the
 * membership flags are cleared again; the caller applies dom[0..n). */
static size_t likho_domain(Solver *s, size_t v, const unsigned char *flipped,
                           unsigned char *in, size_t *dom,
                           size_t max_vars, size_t max_nodes) {
    LikhoSearch d = { s, flipped, in, dom, 1, 0, max_vars, max_nodes };
    dom[0] = v;
    in[v] = 1;
    bool ok = likho_close(&d);
    for (size_t i = 0; i < d.n; i++) in[dom[i]] = 0;  /* failed search leaves n == 1 */
    return ok ? d.n : 0;
}

/* One neutral flip per rogue clause: it breaks nothing, so it only removes
 * clauses from the rogue set. Returns true if anything fired. */
static bool likho_neutral_pass(Solver *s, const size_t *snap, size_t nr,
                               unsigned char *flipped) {
    bool moved = false;
    for (size_t i = 0; i < nr; i++) {
        size_t c = snap[i];
        if (!solver_clause_is_rogue(s, c)) continue;
        const Lit *cl = cnf_clause(s->cnf, c);
        size_t len = cnf_clause_len(s->cnf, c);
        for (size_t k = 0; k < len; k++) {
            size_t v = cl[k].var;
            if (flipped[v]) continue;
            if (solver_refresh_var(s, v).state != VAR_NEUTRAL) continue;
            solver_flip(s, v);
            flipped[v] = 1;
            moved = true;
            break;
        }
    }
    return moved;
}

/* One closed switch domain per rogue clause. A closed domain adds no rogue
 * clause, so this pass can keep going and the next round re-derives the states
 * it changed. Returns true if anything fired. */
static bool likho_domain_pass(Solver *s, const size_t *snap, size_t nr,
                              unsigned char *flipped, unsigned char *in, size_t *dom,
                              size_t max_vars, size_t max_nodes) {
    bool moved = false;
    for (size_t i = 0; i < nr; i++) {
        size_t c = snap[i];
        if (!solver_clause_is_rogue(s, c)) continue;
        const Lit *cl = cnf_clause(s->cnf, c);
        size_t len = cnf_clause_len(s->cnf, c);
        for (size_t k = 0; k < len; k++) {
            size_t v = cl[k].var;
            if (flipped[v]) continue;
            size_t n = likho_domain(s, v, flipped, in, dom, max_vars, max_nodes);
            if (n == 0) continue;
            for (size_t j = 0; j < n; j++) { solver_flip(s, dom[j]); flipped[dom[j]] = 1; }
            moved = true;
            break;
        }
    }
    return moved;
}

bool solver_likho(Solver *s, const SolverConfig *cfg) {
    size_t nv = s->cnf->n_vars, nc = s->cnf->n_clauses;
    size_t max_vars = cfg && cfg->likho_domain_max ? cfg->likho_domain_max : LIKHO_DOMAIN_MAX;
    size_t max_nodes = cfg && cfg->likho_search_nodes ? cfg->likho_search_nodes : LIKHO_SEARCH_NODES;
    bool domains_first = cfg && cfg->likho_domains_first;
    InitMode pre = cfg ? cfg->likho_pre_cook : INIT_ZERO;
    if (max_vars > nv) max_vars = nv;   /* a domain cannot be larger than the formula */
    if (max_vars == 0) max_vars = 1;    /* domain of one: the prime alone, no repair */
    if (pre == INIT_MAJORITY) solver_init_majority(s);
    else if (pre == INIT_RAREST) { if (!solver_init_rarest(s)) return false; }
    else if (pre == INIT_GLOWEST) { if (!solver_init_glowest(s)) return false; }
    else if (pre == INIT_MIN_OVERLAP) { if (!solver_init_min_overlap(s)) return false; }
    else if (pre == INIT_GOB) { if (!solver_init_gob(s)) return false; }
    else if (pre == INIT_GOB_SIGN) { if (!solver_init_gob_sign(s)) return false; }
    else if (pre == INIT_GOB_SQUARE) { if (!solver_init_gob_square(s, NULL)) return false; }
    else if (pre == INIT_GOB_RECULT) { if (!solver_init_gob_recult(s, cfg)) return false; }
    else if (pre == INIT_NEIGHBORHOOD) { if (!solver_init_neighborhood(s, cfg)) return false; }
    else if (pre == INIT_NEIGHBORHOOD0) { if (!solver_init_neighborhood0(s, cfg)) return false; }
    else if (pre == INIT_REVERSE_RAREST) { if (!solver_init_reverse(s, INIT_RAREST)) return false; }
    else if (pre == INIT_REVERSE_GLOWEST) { if (!solver_init_reverse(s, INIT_GLOWEST)) return false; }
    else if (pre == INIT_REVERSE_MIN_OVERLAP) { if (!solver_init_reverse(s, INIT_MIN_OVERLAP)) return false; }
    else {
        memset(s->assignment, 0, nv * sizeof *s->assignment);
        rebuild_counts(s);
    }
    unsigned char *flipped = calloc(nv ? nv : 1, 1);
    unsigned char *in = calloc(nv ? nv : 1, 1);
    size_t *snap = malloc((nc ? nc : 1) * sizeof *snap);
    size_t *dom = malloc(max_vars * sizeof *dom);
    if (!flipped || !in || !snap || !dom) {
        free(flipped); free(in); free(snap); free(dom);
        return false;
    }
    while (s->n_rogue > 0) {
        size_t nr = s->n_rogue;
        memcpy(snap, s->rogue, nr * sizeof *snap);
        /* With -deep-dive the domain pass goes first; either way the other
         * pass runs only when this one found nothing, then the next round
         * re-snapshots so freshly liberated primes are seen as neutral. */
        bool moved = domains_first
            ? likho_domain_pass(s, snap, nr, flipped, in, dom, max_vars, max_nodes)
            : likho_neutral_pass(s, snap, nr, flipped);
        if (!moved)
            moved = domains_first
                ? likho_neutral_pass(s, snap, nr, flipped)
                : likho_domain_pass(s, snap, nr, flipped, in, dom, max_vars, max_nodes);
        if (!moved) break;   /* stuck: no neutral and no switch domain anywhere */
    }
    free(flipped); free(in); free(snap); free(dom);
    return true;
}


/* ---- neighborhood ----------------------------------------------------- */

typedef struct {
    size_t var;
    int8_t tried; /* 1 tried first, 2 tried both */
    int8_t first;
} NbDecision;

static bool nb_clause_sat(const Cnf *cnf, size_t ci, const int8_t *val) {
    const Lit *cl = cnf_clause(cnf, ci);
    size_t len = cnf_clause_len(cnf, ci);
    for (size_t k = 0; k < len; k++) {
        int8_t v = val[cl[k].var];
        if (v < 0) continue;
        if ((v == 1) == cl[k].positive) return true;
    }
    return false;
}

static bool nb_clause_unsat(const Cnf *cnf, size_t ci, const int8_t *val) {
    const Lit *cl = cnf_clause(cnf, ci);
    size_t len = cnf_clause_len(cnf, ci);
    bool any_unset = false;
    for (size_t k = 0; k < len; k++) {
        int8_t v = val[cl[k].var];
        if (v < 0) { any_unset = true; continue; }
        if ((v == 1) == cl[k].positive) return false;
    }
    return !any_unset;
}

static size_t nb_popularity(const Solver *s, size_t var, const unsigned char *active) {
    size_t p = 0;
    for (size_t i = s->occ_off[var]; i < s->occ_off[var + 1]; i++)
        if (active[s->occ[i].clause]) p++;
    return p;
}

bool solver_init_neighborhood(Solver *s, const SolverConfig *cfg) {
    const Cnf *cnf = s->cnf;
    size_t nv = cnf->n_vars, nc = cnf->n_clauses;
    int8_t *val = malloc((nv ? nv : 1) * sizeof *val);
    unsigned char *active = malloc((nc ? nc : 1));
    NbDecision *stack = malloc((nv ? nv : 1) * sizeof *stack);
    if (!val || !active || !stack) { free(val); free(active); free(stack); return false; }
    for (size_t i = 0; i < nv; i++) val[i] = -1;
    for (size_t i = 0; i < nc; i++) active[i] = 1;

    struct timespec deadline = {0, 0};
    bool has_deadline = cfg && cfg->neighborhood_timeout_ms > 0;
    if (has_deadline) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += (time_t)(cfg->neighborhood_timeout_ms / 1000);
        deadline.tv_nsec += (long)((cfg->neighborhood_timeout_ms % 1000) * 1000000UL);
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    }

    size_t sp = 0;
    bool ok = true;
    while (ok) {
        if (nb_timed_out(deadline, has_deadline)) break;
        size_t best = SIZE_MAX, best_pop = 0;
        for (size_t i = 0; i < nv; i++) {
            if (val[i] >= 0) continue;
            size_t pop = nb_popularity(s, i, active);
            if (pop == 0) continue;
            if (best == SIZE_MAX || pop > best_pop || (pop == best_pop && i < best)) { best = i; best_pop = pop; }
        }
        if (best == SIZE_MAX) break;

        int score[2] = {0, 0};
        for (size_t i = s->occ_off[best]; i < s->occ_off[best + 1]; i++) {
            Occ o = s->occ[i];
            if (!active[o.clause] || nb_clause_sat(cnf, o.clause, val)) continue;
            score[o.positive ? 1 : 0]++;
        }
        int8_t first = score[1] >= score[0] ? 1 : 0;
        stack[sp++] = (NbDecision){ best, 1, first };
        val[best] = first;

        bool conflict = false;
        for (size_t i = s->occ_off[best]; i < s->occ_off[best + 1] && !conflict; i++) {
            size_t ci = s->occ[i].clause;
            if (!active[ci] || nb_clause_sat(cnf, ci, val)) continue;
            const Lit *cl = cnf_clause(cnf, ci);
            size_t len = cnf_clause_len(cnf, ci);
            size_t nb_best = SIZE_MAX, nb_pop = 0; int8_t nb_val = 1;
            for (size_t k = 0; k < len; k++) {
                size_t v = cl[k].var;
                if (v == best || val[v] >= 0) continue;
                size_t pop = nb_popularity(s, v, active);
                if (nb_best == SIZE_MAX || pop > nb_pop || (pop == nb_pop && v < nb_best)) { nb_best = v; nb_pop = pop; nb_val = cl[k].positive ? 1 : 0; }
            }
            if (nb_best != SIZE_MAX) {
                if (sp < nv) stack[sp++] = (NbDecision){ nb_best, 1, nb_val };
                val[nb_best] = nb_val;
            }
            if (!nb_clause_sat(cnf, ci, val)) {
                while (sp > 0) {
                    NbDecision *d = &stack[sp - 1];
                    if (d->tried < 2) {
                        d->tried = 2;
                        val[d->var] = d->first ? 0 : 1;
                        bool bad = false;
                        for (size_t j = s->occ_off[d->var]; j < s->occ_off[d->var + 1]; j++)
                            if (nb_clause_unsat(cnf, s->occ[j].clause, val)) { bad = true; break; }
                        if (!bad) { conflict = false; goto nb_retry; }
                    } else { val[d->var] = -1; sp--; }
                }
                ok = false; conflict = true;
            }
        }
nb_retry:
        if (!ok) break;
        for (size_t ci = 0; ci < nc; ci++) active[ci] = !nb_clause_sat(cnf, ci, val);
    }

    for (size_t i = 0; i < nv; i++) s->assignment[i] = (val[i] < 0) ? false : (val[i] == 1);
    rebuild_counts(s);
    free(val); free(active); free(stack);
    return true;
}


/* ---- Neighborhood0 (full spec) ---------------------------------------- */

typedef struct { size_t *v; size_t n, cap; } NbSet;

static bool nbset_add(NbSet *s, size_t x) {
    for (size_t i = 0; i < s->n; i++) if (s->v[i] == x) return true;
    if (s->n == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 8;
        size_t *p = realloc(s->v, ncap * sizeof *p);
        if (!p) return false;
        s->v = p; s->cap = ncap;
    }
    s->v[s->n++] = x;
    return true;
}

typedef struct { size_t *ids; size_t n; } NbOrder;

static void nb0_build_order(const Solver *s, NbOrder *o) {
    size_t nv = s->cnf->n_vars;
    o->ids = malloc((nv ? nv : 1) * sizeof *o->ids);
    o->n = nv;
    if (!o->ids) { o->n = 0; return; }
    for (size_t i = 0; i < nv; i++) o->ids[i] = i;
    /* sort by total occurrence count desc, then id asc */
    for (size_t i = 1; i < nv; i++) {
        size_t key = o->ids[i], j = i;
        size_t kp = occ_count(s, key);
        while (j > 0) {
            size_t jp = occ_count(s, o->ids[j - 1]);
            if (jp > kp || (jp == kp && o->ids[j - 1] < key)) break;
            o->ids[j] = o->ids[j - 1]; j--;
        }
        o->ids[j] = key;
    }
}

typedef struct {
    size_t var;
    int8_t phase; /* 0 set main, 1 neighbors, 2 done */
    int8_t main_val;
    NbSet affected;
    size_t nb_i;
} Nb0Frame;

bool solver_init_neighborhood0(Solver *s, const SolverConfig *cfg) {
    const Cnf *cnf = s->cnf;
    size_t nv = cnf->n_vars, nc = cnf->n_clauses;
    int8_t *val = malloc((nv ? nv : 1) * sizeof *val);
    unsigned char *sat = malloc((nc ? nc : 1));
    Nb0Frame *stack = malloc((nv ? nv : 1) * sizeof *stack);
    NbOrder ord = {0};
    if (!val || !sat || !stack) { free(val); free(sat); free(stack); return false; }
    for (size_t i = 0; i < nv; i++) { val[i] = -1; stack[i] = (Nb0Frame){0}; }
    for (size_t i = 0; i < nc; i++) sat[i] = 0;
    nb0_build_order(s, &ord);
    if (!ord.ids) { free(val); free(sat); free(stack); return false; }

    struct timespec deadline = {0,0}; bool has_deadline = cfg && cfg->neighborhood_timeout_ms > 0;
    if (has_deadline) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += (time_t)(cfg->neighborhood_timeout_ms / 1000);
        deadline.tv_nsec += (long)((cfg->neighborhood_timeout_ms % 1000) * 1000000UL);
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    }

    size_t sp = 0; bool failed = false; size_t cur_idx = 0;
    size_t nb_solved = 0;
    /* initial: pick most popular var overall */
    while (!failed) {
        if (nb_timed_out(deadline, has_deadline)) break;
        /* choose var: if stack top in phase 1, its neighbor; else most popular unset with unsat clauses */
        size_t var = SIZE_MAX;
        if (sp > 0 && stack[sp-1].phase == 1) {
            Nb0Frame *f = &stack[sp-1];
            /* find next unset neighbor in clauses of f->var */
            for (; f->nb_i < occ_count(s, f->var); f->nb_i++) {
                size_t ci = s->occ[s->occ_off[f->var] + f->nb_i].clause;
                if (sat[ci]) continue;
                const Lit *cl = cnf_clause(cnf, ci); size_t len = cnf_clause_len(cnf, ci);
                for (size_t k = 0; k < len; k++) {
                    size_t v = cl[k].var;
                    if (val[v] < 0) { var = v; goto got_var; }
                }
            }
            f->phase = 2; /* neighbors done */
            /* if all clauses of this var set, move to most popular neighbor */
            bool all_set = true;
            for (size_t i = s->occ_off[f->var]; i < s->occ_off[f->var + 1]; i++) if (!sat[s->occ[i].clause]) { all_set = false; break; }
            if (all_set) {
                nb_solved++;
                /* pick most popular neighbor in ord that is unset */
                size_t bestn = SIZE_MAX, bestp = 0;
                for (size_t i = s->occ_off[f->var]; i < s->occ_off[f->var + 1]; i++) {
                    const Lit *cl = cnf_clause(cnf, s->occ[i].clause); size_t len = cnf_clause_len(cnf, s->occ[i].clause);
                    for (size_t k = 0; k < len; k++) {
                        size_t v = cl[k].var; if (val[v] >= 0) continue;
                        size_t p = occ_count(s,v);
                        if (bestn==SIZE_MAX || p>bestp || (p==bestp && v<bestn)) { bestn=v; bestp=p; }
                    }
                }
                if (bestn != SIZE_MAX) var = bestn;
            }
            if (var == SIZE_MAX) {
                /* check global done */
                bool any=false; for(size_t ci=0;ci<nc;ci++) if(!sat[ci]){any=true;break;}
                if(!any) break;
                /* fall through to global pick */
            }
        }
got_var:
        if (var == SIZE_MAX) {
            /* global pick by configured start strategy */
            NeighborhoodStart nstart = cfg ? cfg->neighborhood_start : NB_MOST;
            if (nstart == NB_RANDOM) {
                size_t pool_n = 0;
                for (size_t oi = 0; oi < ord.n; oi++) {
                    size_t v = ord.ids[oi];
                    if (val[v] >= 0) continue;
                    bool has=false; for(size_t i=s->occ_off[v];i<s->occ_off[v+1];i++) if(!sat[s->occ[i].clause]){has=true;break;}
                    if (has) pool_n++;
                }
                if (pool_n > 0) {
                    size_t pick = rng_index(&s->rng, pool_n);
                    for (size_t oi = 0; oi < ord.n; oi++) {
                        size_t v = ord.ids[oi];
                        if (val[v] >= 0) continue;
                        bool has=false; for(size_t i=s->occ_off[v];i<s->occ_off[v+1];i++) if(!sat[s->occ[i].clause]){has=true;break;}
                        if (!has) continue;
                        if (pick == 0) { var = v; break; }
                        pick--;
                    }
                }
            } else {
                size_t bestv = SIZE_MAX, bestp = 0;
                for (size_t oi = 0; oi < ord.n; oi++) {
                    size_t v = ord.ids[oi];
                    if (val[v] >= 0) continue;
                    bool has=false; for(size_t i=s->occ_off[v];i<s->occ_off[v+1];i++) if(!sat[s->occ[i].clause]){has=true;break;}
                    if (!has) continue;
                    size_t p = occ_count(s, v);
                    if (bestv == SIZE_MAX) { bestv=v; bestp=p; }
                    else if (nstart == NB_MOST && (p > bestp || (p == bestp && v < bestv))) { bestv=v; bestp=p; }
                    else if (nstart == NB_LEAST && (p < bestp || (p == bestp && v < bestv))) { bestv=v; bestp=p; }
                }
                var = bestv;
            }
            if (var == SIZE_MAX) break; /* all set or no unsat */
        }

        /* set var: choose value satisfying more active clauses */
        int sc[2]={0,0};
        for (size_t i=s->occ_off[var]; i<s->occ_off[var+1]; i++) {
            Occ o=s->occ[i]; if (sat[o.clause]) continue;
            sc[o.positive?1:0]++;
        }
        int8_t vval = sc[1]>=sc[0]?1:0;
        Nb0Frame *f = &stack[sp];
        f->var=var; f->phase=0; f->main_val=vval; f->nb_i=0;
        f->affected.v=NULL; f->affected.n=0; f->affected.cap=0;
        sp++;
        val[var]=vval;
        /* mark clauses satisfied by var */
        bool conflict=false;
        for (size_t i=s->occ_off[var]; i<s->occ_off[var+1]; i++) {
            size_t ci=s->occ[i].clause;
            if (sat[ci]) continue;
            if (nb_clause_sat(cnf,ci,val)) { if(!nbset_add(&f->affected,ci)) {failed=true;break;} sat[ci]=1; }
        }
        if (failed) break;
        /* check for unsat clauses */
        for (size_t i=s->occ_off[var]; i<s->occ_off[var+1]; i++) {
            if (nb_clause_unsat(cnf, s->occ[i].clause, val)) { conflict=true; break; }
        }
        f->phase = 1; /* proceed to neighbors */
        if (conflict) {
            /* backtrack: per spec - return to neighbor id-1, flip; if fails id-2... */
            bool resolved=false;
            while (sp > 0 && !resolved) {
                Nb0Frame *top = &stack[sp-1];
                /* undo affected sat marks */
                for (size_t i=0;i<top->affected.n;i++) sat[top->affected.v[i]]=0;
                /* flip main var if only tried once? spec: flip neighbor id-1; second fail id-2 */
                /* simple: try flipping this var if not flipped, else pop */
                if (top->main_val >= 0) { /* first attempt, flip */
                    top->main_val = -1; /* mark flipped */
                    val[top->var] = val[top->var] ? 0 : 1;
                    top->affected.n = 0;
                    bool bad=false;
                    for (size_t i=s->occ_off[top->var]; i<s->occ_off[top->var+1]; i++) {
                        size_t ci=s->occ[i].clause;
                        if (nb_clause_sat(cnf,ci,val)) { if(!nbset_add(&top->affected,ci)) {failed=true;break;} sat[ci]=1; }
                        if (nb_clause_unsat(cnf,ci,val)) { bad=true; }
                    }
                    if (!failed && !bad) { top->phase=1; top->nb_i=0; resolved=true; }
                    else { for(size_t i=0;i<top->affected.n;i++) sat[top->affected.v[i]]=0; top->affected.n=0; /* will pop */ }
                }
                if (!resolved) {
                    val[top->var] = -1;
                    free(top->affected.v); top->affected.v=NULL; top->affected.cap=0; top->affected.n=0;
                    sp--;
                }
            }
            if (!resolved) { failed=true; }
        }
        (void)cur_idx;
    }

    for (size_t i = 0; i < nv; i++) s->assignment[i] = (val[i] < 0) ? false : (val[i] == 1);
    rebuild_counts(s);
    for (size_t i=0;i<sp;i++) free(stack[i].affected.v);
    free(val); free(sat); free(stack); free(ord.ids);
    return true;
}

static ResultSolve make_sat(const Solver *s, size_t steps, size_t restart) {
    size_t nv = s->cnf->n_vars;
    bool *a = malloc(nv ? nv : 1);
    if (!a) return ERRF(ResultSolve, ERR_NOMEM, "out of memory");
    if (nv) memcpy(a, s->assignment, nv);
    return OK(ResultSolve, ((SolveResult){ SOLVE_SAT, a, nv, steps, restart, 0, NULL, 0 }));
}

/* Sound UNSAT pre-check: only a unit clause and its negation are contradictory.
 * For length >= 2, a clause and its pointwise negation are satisfiable, e.g.
 * (1 2 3) and (-1 -2 -3) are both true at x1=1, x2=0, x3=0. */
bool cnf_find_contradictions(const Cnf *cnf, Contradiction **out, size_t *out_n) {
    size_t nv = cnf->n_vars;
    size_t *pos = malloc((nv ? nv : 1) * sizeof *pos);
    size_t *neg = malloc((nv ? nv : 1) * sizeof *neg);
    *out = NULL;
    *out_n = 0;
    if (!pos || !neg) { free(pos); free(neg); return false; }
    for (size_t v = 0; v < nv; v++) pos[v] = neg[v] = SIZE_MAX;
    size_t n = 0, cap = 0;
    Contradiction *list = NULL;
    for (size_t ci = 0; ci < cnf->n_clauses; ci++) {
        if (cnf_clause_len(cnf, ci) != 1) continue;
        Lit l = cnf_clause(cnf, ci)[0];
        size_t *same = l.positive ? &pos[l.var] : &neg[l.var];
        size_t *opp = l.positive ? &neg[l.var] : &pos[l.var];
        if (*opp != SIZE_MAX) {
            if (n == cap) {
                size_t ncap = cap ? cap * 2 : 8;
                Contradiction *p = realloc(list, ncap * sizeof *p);
                if (!p) { free(pos); free(neg); free(list); return false; }
                list = p; cap = ncap;
            }
            list[n++] = l.positive ? (Contradiction){ ci, *opp }
                                   : (Contradiction){ *opp, ci };
        }
        *same = ci;
    }
    free(pos); free(neg);
    *out = list;
    *out_n = n;
    return true;
}

static bool cnf_build_reduced(const Cnf *src, const size_t *map, const bool *neg, Cnf *out){
    /* map[v]=SIZE_MAX means keep var (remapped later); else map to map[v] with xor neg[v]. */
    size_t nv=src->n_vars;
    size_t *id=malloc((nv?nv:1)*sizeof*id);
    if(!id) return false;
    size_t nn=0;
    for(size_t v=0;v<nv;v++){ if(map[v]==SIZE_MAX) id[v]=nn++; else id[v]=SIZE_MAX; }
    /* resolve mapping to kept vars */
    size_t *rmap=malloc((nv?nv:1)*sizeof*rmap); bool *rneg=malloc(nv?nv:1);
    if(!rmap||!rneg){ free(id); free(rmap); free(rneg); return false; }
    for(size_t v=0;v<nv;v++){
        size_t cur=v; bool n=false; size_t guard=0;
        while(map[cur]!=SIZE_MAX && guard++<=nv){ n ^= neg[cur]; cur = map[cur]; }
        if(map[cur]!=SIZE_MAX){ free(id); free(rmap); free(rneg); return false; }
        rmap[v]=cur; rneg[v]=n;
    }
    out->n_vars=nn; out->n_clauses=0; out->off=NULL; out->lits=NULL;
    size_t capoff=0,caplit=0;
    if(!vec_reserve((void**)&out->off,&capoff,sizeof*out->off,1)){ free(id); free(rmap); free(rneg); return false; }
    out->off[0]=0;
    Lit *cur=malloc(16*sizeof*cur); size_t ncur=0, capcur=16;
    if(!cur){ free(id); free(rmap); free(rneg); cnf_free(out); return false; }
    for(size_t ci=0;ci<src->n_clauses;ci++){
        ncur=0; bool taut=false;
        const Lit *cl=cnf_clause(src,ci); size_t n=cnf_clause_len(src,ci);
        for(size_t i=0;i<n && !taut;i++){
            size_t v=rmap[cl[i].var]; bool pos = cl[i].positive ^ rneg[cl[i].var];
            Lit nl={(uint32_t)id[v], pos};
            bool dup=false;
            for(size_t k=0;k<ncur;k++){ if(cur[k].var==nl.var){ if(cur[k].positive!=nl.positive) taut=true; else dup=true; break; } }
            if(dup||taut) continue;
            if(ncur==capcur){ size_t nc=capcur*2; Lit*tmp=realloc(cur,nc*sizeof*cur); if(!tmp){ free(cur); free(id); free(rmap); free(rneg); cnf_free(out); return false; } cur=tmp; capcur=nc; }
            cur[ncur++]=nl;
        }
        if(taut) continue;
        size_t start=out->off[out->n_clauses];
        if(!vec_reserve((void**)&out->lits,&caplit,sizeof*out->lits,start+ncur) ||
           !vec_reserve((void**)&out->off,&capoff,sizeof*out->off,out->n_clauses+2)){
            free(cur); free(id); free(rmap); free(rneg); cnf_free(out); return false;
        }
        for(size_t k=0;k<ncur;k++) out->lits[start+k]=cur[k];
        out->off[++out->n_clauses]=start+ncur;
    }
    if(!out->lits) out->lits=malloc(sizeof*out->lits);
    free(cur); free(id); free(rmap); free(rneg);
    return out->lits!=NULL;
}

static ResultSolve solver_solve_path_rider(Solver *s, const SolverConfig *cfg){
    const Cnf *cnf=s->cnf; size_t p=cfg->path_rider; size_t mt=cfg->m_tables?cfg->m_tables:1;
    if(p<2 || p>cnf->n_vars) return ERRF(ResultSolve, ERR_USAGE, "-path-rider P requires 2 <= P <= n_vars");
    if(mt<1) return ERRF(ResultSolve, ERR_USAGE, "-m-tables requires >= 1");
    size_t total = p*mt;
    if(total>cnf->n_vars) return ERRF(ResultSolve, ERR_USAGE, "-path-rider P * -m-tables M must be <= n_vars");
    if((p-1)*mt >= sizeof(size_t)*8) return ERRF(ResultSolve, ERR_USAGE, "-path-rider/-m-tables too large");
    size_t combos = (size_t)1 << ((p-1)*mt);
    Rng rng = rng_new(cfg->seed);
    size_t *pick=malloc(total*sizeof*pick);
    bool *used=calloc(cnf->n_vars?cnf->n_vars:1,1);
    if(!pick||!used){ free(pick); free(used); return ERRF(ResultSolve, ERR_NOMEM, "out of memory"); }
    for(size_t i=0;i<total;i++){
        size_t v;
        do { v = rng_index(&rng, cnf->n_vars); } while(used[v]);
        used[v]=true; pick[i]=v;
    }
    free(used);
    size_t *map=malloc((cnf->n_vars?cnf->n_vars:1)*sizeof*map);
    bool *neg=malloc(cnf->n_vars?cnf->n_vars:1);
    bool *best_a=calloc(cnf->n_vars?cnf->n_vars:1,1);
    if(!map||!neg||!best_a){ free(pick); free(map); free(neg); free(best_a); return ERRF(ResultSolve, ERR_NOMEM, "out of memory"); }
    size_t best_unsat=SIZE_MAX; size_t best_steps=0;
    for(size_t mask=0; mask<combos; mask++){
        for(size_t v=0;v<cnf->n_vars;v++){ map[v]=SIZE_MAX; neg[v]=false; }
        size_t bit=0;
        for(size_t t=0;t<mt;t++){
            size_t base=pick[t*p];
            for(size_t i=1;i<p;i++,bit++){ map[pick[t*p+i]]=base; neg[pick[t*p+i]] = ((mask>>bit)&1u)!=0; }
        }
        Cnf red={0};
        if(!cnf_build_reduced(cnf,map,neg,&red)){ free(pick); free(map); free(neg); free(best_a); return ERRF(ResultSolve, ERR_NOMEM, "out of memory"); }
        SolverConfig sc=*cfg; sc.path_rider=0; sc.m_tables=1;
        Solver *ss=TRY(ResultSolve, solver_new(&red, cfg->seed + mask + 1));
        ResultSolve rs=solver_solve(ss, &sc);
        if(!rs.ok){ solver_free(ss); cnf_free(&red); free(pick); free(map); free(neg); free(best_a); return rs; }
        SolveResult r=rs.val;
        if(r.status==SOLVE_SAT){
            /* map reduced assignment back */
            size_t *id=malloc((cnf->n_vars?cnf->n_vars:1)*sizeof*id); size_t nn=0;
            for(size_t v=0;v<cnf->n_vars;v++){ if(map[v]==SIZE_MAX) id[v]=nn++; else id[v]=SIZE_MAX; }
            bool *full=malloc(cnf->n_vars?cnf->n_vars:1);
            if(!id||!full){ free(id); free(full); solve_result_free(&r); solver_free(ss); cnf_free(&red); free(pick); free(map); free(neg); free(best_a); return ERRF(ResultSolve, ERR_NOMEM, "out of memory"); }
            for(size_t v=0;v<cnf->n_vars;v++){
                size_t cur=v; bool n=false; size_t guard=0;
                while(map[cur]!=SIZE_MAX && guard++<=cnf->n_vars){ n^=neg[cur]; cur=map[cur]; }
                bool val = (cur<cnf->n_vars && id[cur]<r.n_vars) ? r.assignment[id[cur]] : false;
                full[v]= n ? !val : val;
            }
            free(id);
            solve_result_free(&r); solver_free(ss); cnf_free(&red); free(pick); free(map); free(neg); free(best_a);
            return OK(ResultSolve, ((SolveResult){ SOLVE_SAT, full, cnf->n_vars, best_steps, 0, 0, NULL, 0 }));
        }
        if(r.status==SOLVE_UNKNOWN && r.best_unsatisfied < best_unsat){ best_unsat=r.best_unsatisfied; best_steps=r.steps; }
        solve_result_free(&r); solver_free(ss); cnf_free(&red);
    }
    free(pick); free(map); free(neg); free(best_a);
    bool *ua=malloc(cnf->n_vars?cnf->n_vars:1); if(ua){ if(best_a && best_unsat!=SIZE_MAX) memcpy(ua,best_a,cnf->n_vars); else memset(ua,0,cnf->n_vars); }
    return OK(ResultSolve, ((SolveResult){ SOLVE_UNKNOWN, ua, cnf->n_vars, best_steps, 0, best_unsat==SIZE_MAX?cnf->n_clauses:best_unsat, NULL, 0 }));
}

ResultSolve solver_solve(Solver *s, const SolverConfig *cfg) {
    const Cnf *cnf = s->cnf;
    Contradiction *contradictions = NULL;
    size_t n_contradictions = 0;
    if (!cnf_find_contradictions(cnf, &contradictions, &n_contradictions))
        return ERRF(ResultSolve, ERR_NOMEM, "out of memory");
    if (n_contradictions)
        return OK(ResultSolve, ((SolveResult){ SOLVE_UNSAT, NULL, cnf->n_vars, 0, 0, 0,
                                               contradictions, n_contradictions }));
    for (size_t i = 0; i < cnf->n_clauses; i++) {
        if (cnf_clause_len(cnf, i) == 0) {
            bool *ua=calloc(cnf->n_vars?cnf->n_vars:1,1);
            return OK(ResultSolve, ((SolveResult){ SOLVE_UNKNOWN, ua, cnf->n_vars, 0, 0, cnf->n_clauses, NULL, 0 }));
        }
    }
    if (cnf->n_clauses == 0) {
        memset(s->assignment, 0, cnf->n_vars);
        return make_sat(s, 0, 0);
    }
    if (cfg->path_rider > 0) return solver_solve_path_rider(s, cfg);
    PickFn pick = cfg->pick ? cfg->pick : pick_ranked;
    size_t total = 0, best = SIZE_MAX;
    for (size_t restart = 0; restart <= cfg->restarts; restart++) {
        if (restart == 0 && cfg->initial && cfg->init != INIT_KNOTS && cfg->init != INIT_NORTH) {
            if (cnf->n_vars) memcpy(s->assignment, cfg->initial, cnf->n_vars);
            rebuild_counts(s);
        } else if (restart == 0 && cfg->init == INIT_RAREST) {
            if (!solver_init_rarest(s)) return ERRF(ResultSolve, ERR_NOMEM, "out of memory");
        } else if (restart == 0 && cfg->init == INIT_GLOWEST) {
            if (!solver_init_glowest(s)) return ERRF(ResultSolve, ERR_NOMEM, "out of memory");
        } else if (restart == 0 && cfg->init == INIT_YANUS) {
            if (!solver_init_yanus(s, cfg)) return ERRF(ResultSolve, ERR_NOMEM, "out of memory");
        } else if (restart == 0 && (cfg->init == INIT_KNOTS || cfg->init == INIT_NORTH)) {
            bool kok = (cfg->init == INIT_NORTH) ? solver_init_north(s, cfg) : solver_init_knots(s, cfg);
            if (cfg->knots_glowest || cfg->knots_terminator || cfg->knots_likho || cfg->knots_yanus || cfg->knots_gob_recult) {
                if (cfg->knots_glowest) solver_init_glowest(s);
                if (cfg->knots_terminator) solver_terminator(s, cfg->terminator_rounds, cfg->terminator_order);
                if (cfg->knots_likho) solver_likho(s, cfg);
                if (cfg->knots_yanus) solver_init_yanus(s, cfg);
                if (cfg->knots_gob_recult) solver_init_gob_recult(s, cfg);
            }
            if (s->n_rogue != 0) {
                if (cfg->knots_mono) {
                    size_t ci = s->rogue[0];
                    fprintf(stderr, "MONO fail final unsat=%zu first_rogue=%zu\n", s->n_rogue, ci);
                    const Cnf *cc=s->cnf; size_t fn=cnf_clause_len(cc,ci); const Lit*cl=cnf_clause(cc,ci);
                    fprintf(stderr, "MONO failed-block vars="); for(size_t i=0;i<fn;i++) fprintf(stderr,"%s%u",i?" ":"",cl[i].var+1); fprintf(stderr,"\n");
                    size_t bl=0; for(size_t x=0;x<cc->n_clauses;x++){ if(cnf_clause_len(cc,x)!=fn) continue; bool same=true; for(size_t i=0;i<fn;i++){ bool fnd=false; for(size_t j=0;j<fn;j++) if(cnf_clause(cc,x)[j].var==cl[i].var){fnd=true;break;} if(!fnd){same=false;break;} } if(same) bl++; }
                    size_t spc=(fn>=8)?0:((size_t)1<<fn); size_t av=spc>bl?spc-bl:0;
                    fprintf(stderr,"MONO failed-block clauses=%zu space=%zu available=%zu\n",bl,spc,av);
                    fprintf(stderr,"  active:");
                    { bool bs=false; for(size_t i=0;i<fn;i++) fprintf(stderr," %u=%d",cl[i].var+1,s->assignment[cl[i].var]?1:0);
                      for(size_t x=0;x<cc->n_clauses && !bs;x++){ if(cnf_clause_len(cc,x)!=fn) continue; bool same=true; for(size_t i=0;i<fn;i++){ bool fnd=false; for(size_t j=0;j<fn;j++) if(cnf_clause(cc,x)[j].var==cl[i].var){fnd=true;break;} if(!fnd){same=false;break;} } if(same){ const Lit*cy=cnf_clause(cc,x); size_t ny=cnf_clause_len(cc,x); for(size_t k2=0;k2<ny;k2++){ bool val=s->assignment[cy[k2].var]; if(cy[k2].positive?val:!val){bs=true;break;} } } }
                      fprintf(stderr," block_sat=%s\n", bs?"yes":"no"); }
                    for(size_t x=0;x<cc->n_clauses;x++){ if(cnf_clause_len(cc,x)!=fn) continue; bool same=true; for(size_t i=0;i<fn;i++){ bool fnd=false; for(size_t j=0;j<fn;j++) if(cnf_clause(cc,x)[j].var==cl[i].var){fnd=true;break;} if(!fnd){same=false;break;} } if(same){
                        fprintf(stderr,"  clause %zu [",x); const Lit*c2=cnf_clause(cc,x); for(size_t j=0;j<fn;j++) fprintf(stderr,"%s%u%s",c2[j].positive?"":"-",c2[j].var+1,j+1<fn?" ":""); fprintf(stderr,"] variants:");
                        for(unsigned m=0;m<(1u<<fn);m++){
                            bool taut=false, dead=false; for(size_t j=0;j<fn;j++){ int want=(m&(1u<<j))?(c2[j].positive?1:0):(c2[j].positive?0:1); if((want?1:0)==(c2[j].positive?1:0)) taut=true; int cur=s->assignment[c2[j].var]?1:0; if(cur!=want) dead=true; }
                            if(taut) continue;
                            fprintf(stderr," %s{", dead?"dead":"live"); for(size_t j=0;j<fn;j++){ int want=(m&(1u<<j))?(c2[j].positive?1:0):(c2[j].positive?0:1); fprintf(stderr,"%s%u=%d",j?" ":"",c2[j].var+1,want); } fprintf(stderr,"}");
                        }
                        fprintf(stderr,"\n");
                    } }
                    /* external blocks: other var-set blocks sharing any var with failed block */
                    bool *in_fail=calloc(cc->n_vars?cc->n_vars:1,1);
                    if(in_fail){
                        for(size_t i=0;i<fn;i++) in_fail[cl[i].var]=true;
                        bool *seen_sig=calloc(cc->n_clauses?cc->n_clauses:1,1);
                        bool any_ext=false;
                        for(size_t x=0;x<cc->n_clauses;x++){
                            if(seen_sig && seen_sig[x]) continue;
                            const Lit*cx=cnf_clause(cc,x); size_t nx=cnf_clause_len(cc,x);
                            bool shares=false, same_set=(nx==fn);
                            if(same_set){ same_set=true; for(size_t i=0;i<fn;i++){ bool fnd=false; for(size_t j=0;j<nx;j++) if(cx[j].var==cl[i].var){fnd=true;break;} if(!fnd){same_set=false;break;} } }
                            if(same_set){ continue; }
                            for(size_t j=0;j<nx;j++) if(in_fail[cx[j].var]) { shares=true; break; }
                            if(!shares) continue;
                            /* report this external block once by var-set */
                            /* mark all clauses with same var-set as seen */
                            size_t bl2=0; size_t sp2=(nx>=8)?0:((size_t)1<<nx);
                            for(size_t y=0;y<cc->n_clauses;y++){
                                if(cnf_clause_len(cc,y)!=nx) continue;
                                bool same2=true; for(size_t i=0;i<nx;i++){ bool fnd=false; for(size_t j=0;j<nx;j++) if(cnf_clause(cc,y)[j].var==cx[i].var){fnd=true;break;} if(!fnd){same2=false;break;} }
                                if(same2){ bl2++; if(seen_sig) seen_sig[y]=true; }
                            }
                            size_t av2=sp2>bl2?sp2-bl2:0;
                            if(!any_ext){ fprintf(stderr,"MONO external-blocks:\n"); any_ext=true; }
                            fprintf(stderr,"  MONO ext-block vars="); for(size_t j=0;j<nx;j++) fprintf(stderr,"%s%u",j?" ":"",cx[j].var+1);
                            fprintf(stderr," clauses=%zu space=%zu available=%zu\n",bl2,sp2,av2);
                            fprintf(stderr,"    active:");
                            bool block_sat=false;
                            for(size_t j=0;j<nx;j++) fprintf(stderr," %u=%d", cx[j].var+1, s->assignment[cx[j].var]?1:0);
                            for(size_t y=0;y<cc->n_clauses && !block_sat;y++){
                                if(cnf_clause_len(cc,y)!=nx) continue;
                                bool same2=true; for(size_t i=0;i<nx;i++){ bool fnd=false; for(size_t j=0;j<nx;j++) if(cnf_clause(cc,y)[j].var==cx[i].var){fnd=true;break;} if(!fnd){same2=false;break;} }
                                if(same2){
                                    const Lit *cy=cnf_clause(cc,y); size_t ny=cnf_clause_len(cc,y);
                                    block_sat=false;
                                    for(size_t k2=0;k2<ny;k2++){ bool val=s->assignment[cy[k2].var]; if(cy[k2].positive?val:!val){ block_sat=true; break; } }
                                }
                            }
                            fprintf(stderr," block_sat=%s\n", block_sat?"yes":"no");
                            for(size_t y=0;y<cc->n_clauses;y++){
                                if(cnf_clause_len(cc,y)!=nx) continue;
                                bool same2=true; for(size_t i=0;i<nx;i++){ bool fnd=false; for(size_t j=0;j<nx;j++) if(cnf_clause(cc,y)[j].var==cx[i].var){fnd=true;break;} if(!fnd){same2=false;break;} }
                                if(same2){
                                    fprintf(stderr,"    clause %zu [",y); const Lit*c2=cnf_clause(cc,y); for(size_t j=0;j<nx;j++) fprintf(stderr,"%s%u%s",c2[j].positive?"":"-",c2[j].var+1,j+1<nx?" ":""); fprintf(stderr,"] variants:");
                                    for(unsigned m=0;m<(1u<<nx);m++){
                                        bool taut=false, dead=false; for(size_t j=0;j<nx;j++){ int want=(m&(1u<<j))?(c2[j].positive?1:0):(c2[j].positive?0:1); if((want?1:0)==(c2[j].positive?1:0)) taut=true; int cur=s->assignment[c2[j].var]?1:0; if(cur!=want) dead=true; }
                                        if(taut) continue;
                                        fprintf(stderr," %s{", dead?"dead":"live"); for(size_t j=0;j<nx;j++){ int want=(m&(1u<<j))?(c2[j].positive?1:0):(c2[j].positive?0:1); fprintf(stderr,"%s%u=%d",j?" ":"",c2[j].var+1,want); } fprintf(stderr,"}");
                                    }
                                    fprintf(stderr,"\n");
                                }
                            }
                        }
                        if(!any_ext) fprintf(stderr,"MONO external-blocks: none\n");
                        free(seen_sig); free(in_fail);
                    }
                }
                bool *ua=malloc(cnf->n_vars?cnf->n_vars:1); if(ua) memcpy(ua,s->assignment,cnf->n_vars); return OK(ResultSolve, ((SolveResult){ SOLVE_UNKNOWN, ua, cnf->n_vars, 0, 0, s->n_rogue, NULL, 0 }));
            }
            (void)kok;
        } else if (restart == 0 && cfg->init == INIT_MIN_OVERLAP) {
            if (!solver_init_min_overlap(s)) return ERRF(ResultSolve, ERR_NOMEM, "out of memory");
        } else if (restart == 0 && cfg->init == INIT_GOB) {
            if (!solver_init_gob(s)) return ERRF(ResultSolve, ERR_NOMEM, "out of memory");
        } else if (restart == 0 && cfg->init == INIT_GOB_SIGN) {
            if (!solver_init_gob_sign(s)) return ERRF(ResultSolve, ERR_NOMEM, "out of memory");
        } else if (restart == 0 && cfg->init == INIT_GOB_SQUARE) {
            if (!solver_init_gob_square(s, NULL)) return ERRF(ResultSolve, ERR_NOMEM, "out of memory");
        } else if (restart == 0 && cfg->init == INIT_GOB_RECULT) {
            if (!solver_init_gob_recult(s, cfg)) return ERRF(ResultSolve, ERR_NOMEM, "out of memory");
        } else if (restart == 0 && cfg->init == INIT_REVERSE_RAREST) {
            if (!solver_init_reverse(s, INIT_RAREST)) return ERRF(ResultSolve, ERR_NOMEM, "out of memory");
        } else if (restart == 0 && cfg->init == INIT_REVERSE_GLOWEST) {
            if (!solver_init_reverse(s, INIT_GLOWEST)) return ERRF(ResultSolve, ERR_NOMEM, "out of memory");
        } else if (restart == 0 && cfg->init == INIT_REVERSE_MIN_OVERLAP) {
            if (!solver_init_reverse(s, INIT_MIN_OVERLAP)) return ERRF(ResultSolve, ERR_NOMEM, "out of memory");
        } else if (restart == 0 && cfg->init == INIT_TERMINATOR) {
            if (!solver_init_terminator_pre(s, cfg)) return ERRF(ResultSolve, ERR_NOMEM, "out of memory");
        } else if (restart == 0 && cfg->init == INIT_BADDYMOVE) {
            if (!solver_init_baddymove(s, cfg)) return ERRF(ResultSolve, ERR_NOMEM, "out of memory");
        } else if (restart == 0 && cfg->init == INIT_CASCADE) {
            solver_cascade(s);
        } else if (restart == 0 && cfg->init == INIT_NEIGHBORHOOD) {
            if (!solver_init_neighborhood(s, cfg)) return ERRF(ResultSolve, ERR_NOMEM, "out of memory");
        } else if (restart == 0 && cfg->init == INIT_NEIGHBORHOOD0) {
            if (!solver_init_neighborhood0(s, cfg)) return ERRF(ResultSolve, ERR_NOMEM, "out of memory");
        } else if (restart == 0 && cfg->init == INIT_M_PLUS) {
            if (!solver_init_m_plus(s)) return ERRF(ResultSolve, ERR_NOMEM, "out of memory");
        } else if (restart == 0 && cfg->init == INIT_LIKHO) {
            if (!solver_likho(s, cfg)) return ERRF(ResultSolve, ERR_NOMEM, "out of memory");
        } else {
            random_assignment(s, restart == 0);
        }
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
    bool *ua = malloc(cnf->n_vars ? cnf->n_vars : 1);
    if (ua) memcpy(ua, s->assignment, cnf->n_vars);
    return OK(ResultSolve, ((SolveResult){ SOLVE_UNKNOWN, ua, cnf->n_vars, total, 0, best, NULL, 0 }));
}

void solve_result_free(SolveResult *r) {
    free(r->assignment);
    free(r->contradictions);
    r->assignment = NULL;
    r->contradictions = NULL;
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

void solver_terminator(Solver *s, size_t rounds, TerminatorOrder order) {
    size_t nv = s->cnf->n_vars;
    size_t *perm = NULL;
    if (order == TERM_RANDOM && nv > 0) {
        perm = malloc(nv * sizeof *perm);
        if (perm) for (size_t i = 0; i < nv; i++) perm[i] = i;
        else order = TERM_FORWARD;   /* shortcut: alloc failure falls back to forward */
    }
    for (size_t r = 0; r < rounds && s->n_rogue > 0; r++) {
        if (order == TERM_RANDOM) {
            for (size_t i = nv; i > 1; i--) {
                size_t j = rng_index(&s->rng, i);
                size_t t = perm[i - 1]; perm[i - 1] = perm[j]; perm[j] = t;
            }
        }
        size_t flips = 0;
        for (size_t k = 0; k < nv; k++) {
            size_t i = (order == TERM_REVERSE) ? (nv - 1 - k)
                     : (order == TERM_RANDOM) ? perm[k] : k;
            refresh_var(s, i);
            const VarStats *st = &s->stats[i];
            if (st->make_count > st->break_count) { solver_flip(s, i); flips++; }
        }
        if (flips == 0) break;
    }
    free(perm);
}

void solver_baddymove(Solver *s, size_t rounds, TerminatorOrder order, double allow) {
    size_t nv = s->cnf->n_vars;
    size_t *perm = NULL;
    if (allow < 0.0) allow = 0.0;
    if (allow > 1.0) allow = 1.0;
    if (order == TERM_RANDOM && nv > 0) {
        perm = malloc(nv * sizeof *perm);
        if (perm) for (size_t i = 0; i < nv; i++) perm[i] = i;
        else order = TERM_FORWARD;
    }
    for (size_t r = 0; r < rounds && s->n_rogue > 0; r++) {
        if (order == TERM_RANDOM) {
            for (size_t i = nv; i > 1; i--) {
                size_t j = rng_index(&s->rng, i);
                size_t t = perm[i - 1]; perm[i - 1] = perm[j]; perm[j] = t;
            }
        }
        size_t flips = 0;
        for (size_t k = 0; k < nv; k++) {
            size_t i = (order == TERM_REVERSE) ? (nv - 1 - k)
                     : (order == TERM_RANDOM) ? perm[k] : k;
            refresh_var(s, i);
            const VarStats *st = &s->stats[i];
            bool good = st->make_count > st->break_count;
            bool bad = st->break_count > st->make_count;
            bool take = good || (bad && allow > 0.0 && rng_f64(&s->rng) < allow);
            if (take) { solver_flip(s, i); flips++; }
        }
        if (flips == 0) break;
    }
    free(perm);
}

static bool solver_apply_pre_cook(Solver *s, const SolverConfig *cfg, InitMode pre) {
    if (pre == INIT_MAJORITY) solver_init_majority(s);
    else if (pre == INIT_RAREST) { if (!solver_init_rarest(s)) return false; }
    else if (pre == INIT_GLOWEST) { if (!solver_init_glowest(s)) return false; }
    else if (pre == INIT_MIN_OVERLAP) { if (!solver_init_min_overlap(s)) return false; }
    else if (pre == INIT_GOB) { if (!solver_init_gob(s)) return false; }
    else if (pre == INIT_GOB_SIGN) { if (!solver_init_gob_sign(s)) return false; }
    else if (pre == INIT_GOB_SQUARE) { if (!solver_init_gob_square(s, NULL)) return false; }
    else if (pre == INIT_GOB_RECULT) { if (!solver_init_gob_recult(s, cfg)) return false; }
    else if (pre == INIT_NEIGHBORHOOD) { if (!solver_init_neighborhood(s, cfg)) return false; }
    else if (pre == INIT_NEIGHBORHOOD0) { if (!solver_init_neighborhood0(s, cfg)) return false; }
    else if (pre == INIT_REVERSE_RAREST) { if (!solver_init_reverse(s, INIT_RAREST)) return false; }
    else if (pre == INIT_REVERSE_GLOWEST) { if (!solver_init_reverse(s, INIT_GLOWEST)) return false; }
    else if (pre == INIT_REVERSE_MIN_OVERLAP) { if (!solver_init_reverse(s, INIT_MIN_OVERLAP)) return false; }
    else {
        memset(s->assignment, 0, s->cnf->n_vars * sizeof *s->assignment);
        rebuild_counts(s);
    }
    return true;
}

void solver_init_terminator(Solver *s, size_t rounds, TerminatorOrder order) {
    solver_init_majority(s);
    solver_terminator(s, rounds, order);
}

bool solver_init_terminator_pre(Solver *s, const SolverConfig *cfg) {
    InitMode pre = cfg ? cfg->likho_pre_cook : INIT_MAJORITY;
    if (!solver_apply_pre_cook(s, cfg, pre)) return false;
    solver_terminator(s, cfg ? cfg->terminator_rounds : 16, cfg ? cfg->terminator_order : TERM_FORWARD);
    return true;
}

bool solver_init_baddymove(Solver *s, const SolverConfig *cfg) {
    InitMode pre = cfg ? cfg->likho_pre_cook : INIT_MAJORITY;
    if (!solver_apply_pre_cook(s, cfg, pre)) return false;
    solver_baddymove(s, cfg ? cfg->terminator_rounds : 16, cfg ? cfg->terminator_order : TERM_FORWARD,
                     cfg ? cfg->baddymove_allow : 0.0);
    return true;
}

void solver_cascade(Solver *s) {
    const Cnf *cnf = s->cnf;
    size_t nv = cnf->n_vars;
    size_t *seen = calloc(nv ? nv : 1, sizeof *seen);
    if (!seen) return; /* shortcut: alloc failure, keep assignment as-is */
    size_t stamp = 0;
    for (size_t v = 0; v < nv; v++) {
        size_t M = 0, _M = 0, m = 0, _m = 0;
        /* M/m: clauses sat by x=v true, m = distinct vars that can sat same clauses */
        stamp++; if (!stamp) { memset(seen, 0, (nv ? nv : 1) * sizeof *seen); stamp = 1; }
        for (size_t i = s->occ_off[v]; i < s->occ_off[v + 1]; i++) {
            Occ o = s->occ[i];
            if (!o.positive) continue;
            M++;
            const Lit *cl = cnf_clause(cnf, o.clause);
            size_t len = cnf_clause_len(cnf, o.clause);
            for (size_t k = 0; k < len; k++) {
                size_t u = cl[k].var;
                if (u == v || seen[u] == stamp) continue;
                seen[u] = stamp; m++;
            }
        }
        /* _M/_m: clauses sat by x=v false */
        stamp++; if (!stamp) { memset(seen, 0, (nv ? nv : 1) * sizeof *seen); stamp = 1; }
        for (size_t i = s->occ_off[v]; i < s->occ_off[v + 1]; i++) {
            Occ o = s->occ[i];
            if (o.positive) continue;
            _M++;
            const Lit *cl = cnf_clause(cnf, o.clause);
            size_t len = cnf_clause_len(cnf, o.clause);
            for (size_t k = 0; k < len; k++) {
                size_t u = cl[k].var;
                if (u == v || seen[u] == stamp) continue;
                seen[u] = stamp; _m++;
            }
        }
        double left = (double)M / (double)(m ? m : 1);
        double right = (double)_M / (double)(_m ? _m : 1);
        s->assignment[v] = left > right;
    }
    free(seen);
    rebuild_counts(s);
}

void solver_init_majority(Solver *s) { random_assignment(s, true); }
const size_t *solver_rogue_list(const Solver *s) { return s->rogue; }
VarStats solver_refresh_var(Solver *s, size_t v) { refresh_var(s, v); return s->stats[v]; }
size_t solver_occ_count(const Solver *s, size_t v) { return occ_count(s, v); }
size_t solver_occ_clause(const Solver *s, size_t v, size_t i) { return s->occ[s->occ_off[v] + i].clause; }

void solver_set_assignment(Solver *s, const bool *a) {
    if (s->cnf->n_vars) memcpy(s->assignment, a, s->cnf->n_vars);
    rebuild_counts(s);
}
