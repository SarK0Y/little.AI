#!/usr/bin/env python3
"""Random 3-CNF generator: gen_cnf.py N_VARS RATIO [SEED] > out.cnf
Each clause has 3 distinct variables with random polarity. The hard region for
random 3-SAT is around RATIO ~= 4.26."""
import random, sys

n = int(sys.argv[1]); ratio = float(sys.argv[2])
rng = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 0)
m = round(n * ratio)
print(f"c random 3-CNF n={n} m={m}")
print(f"p cnf {n} {m}")
for _ in range(m):
    vs = rng.sample(range(1, n + 1), 3)
    print(" ".join(str(v if rng.random() < 0.5 else -v) for v in vs), 0)
