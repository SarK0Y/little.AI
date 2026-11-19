#!/usr/bin/env python3
"""Diagnose the CNF -> linear-system projection used by gob/gob-sign.

usage: projection_counts.py FILE.cnf [FILE.cnf ...]
         parsed clauses vs equations after no_twins, ranks, least-squares RMS
       projection_counts.py --signs A.txt B.txt
         differing signs from two `probe start` dumps
"""
import sys


def parsed_clauses(path):
    """Mirror cnf_from_dimacs_str: drop tautologies, shorten repeated literals."""
    with open(path) as f:
        lines = f.read().splitlines()
    header = next(line for line in lines if line.startswith('p ')).split()
    nv = int(header[2])
    out = []
    for line in lines:
        if not line.strip() or line[0] in 'pc%':
            continue
        seen, cl = {}, []
        for tok in line.split()[:-1]:
            lit = int(tok)
            var, positive = abs(lit), lit > 0
            if var in seen and seen[var] != positive:
                cl = []
                break
            seen[var] = positive
            cl.append(var if positive else -var)
        if cl:
            out.append(cl)
    return nv, out


def twin_filter(cls):
    """Mirror no_twins: keep the first of each sync/async twin pair."""
    keep, seen = [], set()
    for cl in cls:
        a, b = tuple(sorted(cl)), tuple(sorted(-l for l in cl))
        if a in seen or b in seen:
            continue
        seen.add(a)
        seen.add(b)
        keep.append(cl)
    return keep


def report(path):
    import numpy as np  # only needed for ranks

    nv, cls = parsed_clauses(path)
    eqs = twin_filter(cls)
    m = len(eqs)
    A = np.zeros((m, nv))
    for i, cl in enumerate(eqs):
        for lit in cl:
            A[i, abs(lit) - 1] = 1.0 if lit > 0 else -1.0
    b = np.ones(m)
    x, *_ = np.linalg.lstsq(A, b, rcond=None) if m and nv else (np.zeros(nv),)
    rms = float(np.sqrt(np.mean((A @ x - b) ** 2))) if m else 0.0
    print(f"{path}: clauses={len(cls)} equations={m} n={nv} "
          f"rankA={np.linalg.matrix_rank(A)} rankAb={np.linalg.matrix_rank(np.column_stack([A, b]))} "
          f"redundant={m - np.linalg.matrix_rank(A)} lsRMS={rms:.3f}")


def sign_diff(a, b):
    with open(a) as f:
        x = f.read().strip()
    with open(b) as f:
        y = f.read().strip()
    if len(x) != len(y):
        sys.exit('sign dumps differ in length')
    d = sum(p != q for p, q in zip(x, y))
    print(f"{a} vs {b}: {d}/{len(x)} signs differ ({100 * d / len(x):.2f}%)")


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--signs':
        sign_diff(sys.argv[2], sys.argv[3])
    else:
        for path in sys.argv[1:]:
            report(path)
