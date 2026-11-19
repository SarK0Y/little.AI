#!/usr/bin/env python3
"""Permute the clause lines of a DIMACS CNF: same clauses, different row order.

usage: permute_clauses.py IN.cnf OUT.cnf SEED
"""
import random
import sys


def main():
    src, dst, seed = sys.argv[1], sys.argv[2], int(sys.argv[3])
    with open(src) as f:
        lines = f.read().splitlines()
    header = lines[0].split()
    if len(header) < 4 or header[0] != 'p' or header[1] != 'cnf':
        sys.exit('expected DIMACS p cnf header')
    clauses = [line for line in lines[1:] if line.strip()]
    if any(line.split()[-1] != '0' for line in clauses):
        sys.exit('bad clause line')
    random.Random(seed).shuffle(clauses)
    with open(dst, 'w') as f:
        f.write(f"p cnf {header[2]} {len(clauses)}\n")
        f.write("\n".join(clauses) + "\n")


if __name__ == '__main__':
    main()
