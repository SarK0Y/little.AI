#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sat.h"

static int clause_status(const Cnf *cnf, size_t ci, const signed char *a, size_t *unit_var, int *unit_val) {
    const Lit *cl = cnf_clause(cnf, ci);
    size_t n = cnf_clause_len(cnf, ci);
    size_t unassigned = 0;
    Lit ul = {0};
    for (size_t k = 0; k < n; k++) {
        int v = a[cl[k].var];
        if (v == -1) { unassigned++; ul = cl[k]; }
        else if (v == (int)cl[k].positive) return 1; /* satisfied */
    }
    if (unassigned == 0) return -1; /* conflict */
    if (unassigned == 1) { *unit_var = ul.var; *unit_val = ul.positive ? 1 : 0; return 0; }
    return 2; /* unresolved */
}

static int unit_propagate(const Cnf *cnf, signed char *a) {
    for (;;) {
        int changed = 0;
        for (size_t ci = 0; ci < cnf->n_clauses; ci++) {
            size_t uv; int uval;
            int st = clause_status(cnf, ci, a, &uv, &uval);
            if (st == 1 || st == 2) continue;
            if (st == -1) return 0;
            if (a[uv] == -1) { a[uv] = (signed char)uval; changed = 1; }
            else if (a[uv] != uval) return 0;
        }
        if (!changed) return 1;
    }
}

static int all_satisfied(const Cnf *cnf, const signed char *a, size_t *pick) {
    *pick = SIZE_MAX;
    for (size_t ci = 0; ci < cnf->n_clauses; ci++) {
        const Lit *cl = cnf_clause(cnf, ci);
        size_t n = cnf_clause_len(cnf, ci);
        int sat = 0;
        for (size_t k = 0; k < n; k++) {
            int v = a[cl[k].var];
            if (v != -1 && v == (int)cl[k].positive) { sat = 1; break; }
        }
        if (!sat) {
            if (*pick == SIZE_MAX) {
                for (size_t k = 0; k < n; k++)
                    if (a[cl[k].var] == -1) { *pick = cl[k].var; break; }
            }
            if (*pick == SIZE_MAX) return 0; /* conflict */
            return 0;
        }
    }
    return 1;
}

static int dpll(const Cnf *cnf, signed char *a) {
    if (!unit_propagate(cnf, a)) return 0;
    size_t pick;
    if (all_satisfied(cnf, a, &pick)) return 1;
    if (pick == SIZE_MAX) return 0;
    for (int val = 1; val >= 0; val--) {
        signed char *b = malloc(cnf->n_vars ? cnf->n_vars : 1);
        if (!b) return 0;
        memcpy(b, a, cnf->n_vars);
        b[pick] = (signed char)val;
        if (dpll(cnf, b)) { memcpy(a, b, cnf->n_vars); free(b); return 1; }
        free(b);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s file.cnf [...]\n", argv[0]);
        return 2;
    }
    int any_fail = 0;
    for (int i = 1; i < argc; i++) {
        ResultCnf rc = cnf_from_dimacs_file(argv[i]);
        if (!rc.ok) {
            fprintf(stderr, "%s: parse error: %s\n", argv[i], rc.err.msg);
            any_fail = 1; continue;
        }
        Cnf cnf = rc.val;
        signed char *a = malloc(cnf.n_vars ? cnf.n_vars : 1);
        if (!a) { fprintf(stderr, "%s: OOM\n", argv[i]); cnf_free(&cnf); return 1; }
        for (size_t v = 0; v < cnf.n_vars; v++) a[v] = -1;
        int sat = dpll(&cnf, a);
        printf("FILE %s\n", argv[i]);
        if (!sat) {
            printf("STATUS UNSAT\n");
            any_fail = 1;
        } else {
            bool *ba = malloc(cnf.n_vars ? cnf.n_vars : 1);
            if (!ba) { fprintf(stderr, "%s: OOM\n", argv[i]); free(a); cnf_free(&cnf); return 1; }
            for (size_t v = 0; v < cnf.n_vars; v++) ba[v] = (a[v] == -1) ? false : (a[v] != 0);
            int ok = verify(&cnf, ba, cnf.n_vars);
            printf("STATUS %s\n", ok ? "SAT" : "BAD");
            printf("VARS %zu CLAUSES %zu\n", cnf.n_vars, cnf.n_clauses);
            printf("MODEL");
            for (size_t v = 0; v < cnf.n_vars; v++) printf(" %s%zu", ba[v] ? "" : "-", v + 1);
            printf(" 0\n");
            if (!ok) any_fail = 1;
            free(ba);
        }
        free(a);
        cnf_free(&cnf);
    }
    return any_fail ? 1 : 0;
}
