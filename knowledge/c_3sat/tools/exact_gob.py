#!/usr/bin/env python3
"""Pure Gob: solve the projection exactly over Q instead of iterating Kaczmarz.

usage: exact_gob.py [--consistent] FILE.cnf TRUTH.json

TRUTH.json is a SAT result from the C solver (`3sat ... --out TRUTH.json`).
By default the target is b = 1, the gob projection. With --consistent the target
is b = A t for the known boolean solution t, which makes A x = b consistent.
Prints:
  * whether A x = b is consistent (exact Gauss-Jordan over rationals)
  * sign of the exact least-squares point (A^T A)^-1 A^T b (unique, order-free)
  * sign of 64-sweep Kaczmarz under several row orders
  * sign of an exact full-rank n-of-m subsystem under the same orders
Small instances only (n <= ~200): exact arithmetic is O(n^3).
"""
import json
import sys
from fractions import Fraction as F


def parse(path):
    with open(path) as f:
        lines = f.read().splitlines()
    header = next(line for line in lines if line.startswith('p ')).split()
    nv = int(header[2])
    cls = []
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
            cls.append(cl)
    return nv, cls


def solve_exact(rows, rhs):
    """Gauss-Jordan over Q. Returns (solution, rank); solution is None if inconsistent."""
    n, m = len(rows[0]), len(rows)
    M = [[F(v) for v in row] + [F(b)] for row, b in zip(rows, rhs)]
    pivots, r = [], 0
    for c in range(n):
        p = next((i for i in range(r, m) if M[i][c]), None)
        if p is None:
            continue
        M[r], M[p] = M[p], M[r]
        pv = M[r][c]
        M[r] = [v / pv for v in M[r]]
        for i in range(m):
            if i != r and M[i][c]:
                f = M[i][c]
                M[i] = [a - f * b for a, b in zip(M[i], M[r])]
        pivots.append(c)
        r += 1
        if r == m:
            break
    for i in range(r, m):
        if not any(M[i][:n]) and M[i][n]:
            return None, r
    x = [F(0)] * n
    for i, c in enumerate(pivots):
        x[c] = M[i][n]
    return x, r


def full_rank_indices(rows, nv):
    """Indices of the first nv independent rows of an order (exact elimination)."""
    basis, chosen = [], []
    for idx, row in enumerate(rows):
        r = [F(v) for v in row]
        for c, br in basis:
            if r[c]:
                f = r[c]
                r = [a - f * b for a, b in zip(r, br)]
        p = next((i for i, v in enumerate(r) if v), None)
        if p is None:
            continue
        inv = 1 / r[p]
        r = [v * inv for v in r]
        basis.append((p, r))
        basis.sort(key=lambda item: item[0])
        chosen.append(idx)
        if len(chosen) == nv:
            break
    return chosen


def kaczmarz(cls, nv, order, rhs, sweeps=64):
    v = [0.0] * nv
    for _ in range(sweeps):
        for ci in order:
            cl = cls[ci]
            s = sum(v[abs(l) - 1] if l > 0 else -v[abs(l) - 1] for l in cl)
            step = (rhs[ci] - s) / len(cl)
            for l in cl:
                v[abs(l) - 1] += step if l > 0 else -step
    return v


def diff(a, b):
    return sum(p != q for p, q in zip(a, b))


def main():
    consistent = len(sys.argv) > 1 and sys.argv[1] == '--consistent'
    args = sys.argv[2:] if consistent else sys.argv[1:]
    cnf_path, truth_path = args[0], args[1]
    nv, cls = parse(cnf_path)
    m = len(cls)
    A = [[0] * nv for _ in range(m)]
    for i, cl in enumerate(cls):
        for lit in cl:
            A[i][abs(lit) - 1] = 1 if lit > 0 else -1

    truth = [v == 'true' for v in json.load(open(truth_path))['assignment']]
    print(f"\n{cnf_path}: m={m} n={nv} target={'A*t (consistent)' if consistent else '1 (gob)'}")

    if consistent:
        t = [1 if v else -1 for v in truth]
        rhs = [sum(A[i][j] * t[j] for j in range(nv)) for i in range(m)]
    else:
        rhs = [1] * m
    G = [[sum(A[k][i] * A[k][j] for k in range(m)) for j in range(nv)] for i in range(nv)]
    c = [sum(A[k][i] * rhs[k] for k in range(m)) for i in range(nv)]
    xls, rank = solve_exact(G, c)
    s_ls = [v > 0 for v in xls]
    ax = sorted(abs(v) for v in xls)
    print(f"  exact LS: rank(G)={rank}  agrees with SAT {nv - diff(s_ls, truth)}/{nv}  "
          f"|x| median={float(ax[nv // 2]):.4f} max={float(ax[-1]):.4f}")

    full, rk = solve_exact(A, rhs)
    print(f"  full system A x = b: {'FOUND' if full else 'INCONSISTENT'} (rank {rk})")

    orders = [list(range(m))]
    # Deterministic permutations, no numpy dependency.
    for p in (7, 11, 13):
        state = p
        order = list(range(m))
        for i in range(m - 1, 0, -1):
            state = (state * 1103515245 + 12345) & 0x7FFFFFFF
            j = state % (i + 1)
            order[i], order[j] = order[j], order[i]
        orders.append(order)

    kacz_signs, square_signs = [], []
    for k, order in enumerate(orders):
        rows = [A[i] for i in order]
        sk = [v > 0 for v in kaczmarz(cls, nv, order, rhs)]
        kacz_signs.append(sk)
        selected = full_rank_indices(rows, nv)
        xs, sr = solve_exact([rows[j] for j in selected], [rhs[order[j]] for j in selected])
        ss = [v > 0 for v in xs] if xs else None
        square_signs.append(ss)
        print(f"  order {k}: Kaczmarz vs LS {diff(sk, s_ls):3d}, vs SAT {diff(sk, truth):3d} | "
              f"exact square rank {sr}, sign vs LS {diff(ss, s_ls) if ss else 'n/a':>3}, "
              f"vs SAT {diff(ss, truth) if ss else 'n/a':>3}")
    print(f"  sign drift across orders: Kaczmarz {[diff(s, kacz_signs[0]) for s in kacz_signs]}, "
          f"exact square {[diff(s, square_signs[0]) for s in square_signs]}")


if __name__ == '__main__':
    main()
