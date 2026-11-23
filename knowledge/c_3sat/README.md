# c_3sat

C port of `knowledge/3sat` (the Rust `context-3sat` experiment): an incremental
stochastic local-search 3-SAT engine for `little.AI` context handling.

Dependency-free C11 (GNU extensions: GCC/Clang). It returns **SAT** (verified
against the input) or **UNKNOWN** when the budget runs out. Local search cannot
prove UNSAT, so the only UNSAT it reports is structural: a unit clause `(x)` and
its negation `(-x)` are detected up front and returned as **UNSAT** with the list
of contradictory clause pairs. A pair of longer negated clauses is *not*
contradictory (`(1 2 3)` and `(-1 -2 -3)` are both true at `x1=1, x2=0, x3=0`).

## Build / test

```sh
make -C knowledge/c_3sat build   # -> build/3sat
make -C knowledge/c_3sat test    # ASan + UBSan test run
make -C knowledge/c_3sat bench N=20000 RATIO=3.5 SEED=1
```

## Run

```sh
knowledge/c_3sat/build/3sat input.cnf
knowledge/c_3sat/build/3sat input.cnf --steps 250000 --restarts 40 --noise 0.20 --seed 42
knowledge/c_3sat/build/3sat input.cnf --strategy greedy --progress 100000 --out out.json
knowledge/c_3sat/build/3sat input.cnf -deep-dive 32 128
knowledge/c_3sat/build/3sat input.cnf -pre-cook majority
```

Default output: `./knowledge/c_3sat/results/latest.json` (relative to cwd), same
JSON shape as the Rust version. Exit codes: `0` SAT, `1` input/output/internal
error, `2` usage error, `3` budget exhausted (UNKNOWN), `4` structural UNSAT.

UNSAT output lists the conflicting clause indices (0-based):

```json
{
  "status": "UNSAT",
  "message": "UNSAT",
  "variables": 3,
  "clauses": 5,
  "contradictions": [{"i": 1, "j": 3}]
}
```

Here clause `j` is the negation of clause `i`; both must be unit clauses.
Non-unit relatives -- e.g. `(1 2)` and `(-1 -2)` -- are consistent even when the
formula has more clauses than variables, so they are not reported.

## Design

**Rust-like errors** (`src/result.h`). Fallible functions return a tagged
`Result` instead of using errno or sentinel values:

```c
typedef RESULT(Cnf) ResultCnf;                 // { bool ok; union { Cnf val; Error err; } }
Cnf cnf = TRY_CTX(ResultInt, cnf_from_dimacs_file(path), "Failed to load CNF");  // like `?` + .context()
return ERRF(ResultUnit, ERR_PARSE, "line %zu: ...", n);                           // like Err(...)
```

`TRY`, `TRY_CTX` (adds context) and `TRY0` (for `Result<()>`) early-return the
error from the enclosing function. Named typedefs are required per result type
(two anonymous structs are different types in C).

**Functions as variables.**

- `PickFn` chooses the variable to flip from the scored candidates. It is stored
  in `SolverConfig.pick`; built-ins are `ranked` (default, the original
  heuristic), `greedy`, `random`, selected by `--strategy` through the
  `STRATEGIES` name table.
- `ProgressFn` is an optional observer called every N flips (`--progress N`).
- CLI flags are a table of `{flag, handler}` function pointers (`OPTS` in `main.c`).

Adding a heuristic = write a `PickFn`, add one row to `STRATEGIES`.

**Speed / energy.**

- Flat CSR arrays for clauses and variable occurrences (no per-clause allocations).
- Variable states (prime/neutral, break/make counts) are derived *lazily*, only for
  the 2-3 variables of the clause actually examined, in one pass. A flip only
  updates clause counters and the rogue-clause index. On random 3-SAT this is
  ~10x faster than re-ranking every variable in every touched clause, with
  identical search decisions (same seed -> same steps).

## Initial assignment (`--init`)

Restart 0 starts from a chosen init (later restarts are always random):

- `--init majority` (default): every variable takes its majority polarity.
- `--init rarest`: deterministic constructive start (no RNG), rarest variables first.
- `--init glowest`: deterministic constructive start, most frequent variables first.
- `--init yanus`: random variable-expansion restarts of glowest; each attempt
  adds `D` fresh variables and `R` random definitions `x = (±y_a) ∨ (±y_b)`
  (signs picked from the RNG), runs glowest on the expanded CNF, and maps the
  original variables back. `D` and `R` are drawn from the RNG each attempt
  (unless `-saturate F` fixes `D = F · n_clauses`); `-timeout N` (default 8) caps attempts
  (`0` = plain glowest). Early-exits on SAT, otherwise keeps the mapped start
  with the fewest rogue clauses.
- `--init min-overlapping`: deterministic constructive start, first the most
  frequent variable, then the one overlapping the solved part least.
- `--init gob`: deterministic linear-relaxation start (no RNG), clauses as signed
  equations solved by Kaczmarz sweeps and decoded by magnitude parity.
- `--init gob-sign`: the same system, decoded by sign instead of magnitude.
- `--init gob-square`: the same, but only `n` of the `m` equations are solved
  (a square subsystem); it reports how many of the remaining equations the
  solution misses.
- `--init gob-recult`: the clause system augmented with tail variables `Y` on the
  right-hand side (recultivation), solved by Kaczmarz; `x` is decoded by sign and
  repaired and `Y` is dropped. By default a BaddyMove partial assignment gives a
  center `B(b) = Σ_i (−1)^i (b_i − 1)` (`b_i` = number of true literals in clause
  `i`); the tail-sum baseline `B` is then brute-forced in
  `[B(b) − k·s, B(b) + k·s]`, `s = min(n/4+1, 128)` (`-coef-k K`, default 2), keeping the
  decoded assignment with the fewest rogue clauses. `-play-baseline P`
  sets `B = rng mod P` (`P >= 1`), `-set-baseline N` fixes `B = N` (if both given,
  `-set-baseline` wins); either explicit flag skips the brute-force.
- `--init reverse-rarest|reverse-glowest|reverse-min-overlapping`: negate every
  clause, run the named constructive start on the reversed system, negate the
  assignment back (“vice versa”).
- `--init terminator`: pre-cook start (majority by default), then improvement
  rounds of strictly gainful flips (`make_count > break_count`), re-evaluated per
  variable. `--terminator-rounds N` (default 16) caps passes; `--terminator-order
  forward|reverse|random` sets the per-round visit order (random uses the solver
  RNG and reshuffles each round). Combine with `-pre-cook ...` to seed terminator
  from another init (e.g. `-pre-cook neighborhood0`).
- `--init neighborhood|neighborhood0`: popularity/backtracking neighborhood
  search. `neighborhood0` is the full spec variant (most-popular var, satisfy its
  clause neighborhood via neighbors, propagate to most popular neighbor,
  backtrack/flip on conflict); `neighborhood` is a lighter approximation.
  `--neighborhood-timeout MS` (default 1000) bounds backtracking;
  `--neighborhood-start most|least|random` picks the global start/next var
  (`random` uses the solver RNG).
- `--init likho`: pre-cooked start (all-false by default), then a
  neutral/switch-domain repair pass.
- `-deep-dive VARS NODES`: implies `--init likho`, caps switch domains at VARS
  variables, gives each prime at most NODES search nodes, and fires domains
  before neutral repairs.
- `-pre-cook zero|majority|rarest|glowest|min-overlapping|gob|gob-sign|gob-square|gob-recult|neighborhood|neighborhood0|reverse-rarest|reverse-glowest|reverse-min-overlapping`:
  implies `--init likho` unless `--init terminator` is set; phase 1 is that
  assignment instead of the default all-false/majority one (a partially solved
  input).

All inits work in both `plain` and `groups` mode; `-deep-dive` is a variant of
`likho`.

### Rarest / Glowest / Min-overlapping

All three use the same value rule: the picked variable takes the value that
satisfies more **forced** clauses (open clauses where it is the last unset
variable); else more **open** (not yet satisfied) clauses; else the larger total
polarity; else `false`.

They differ only in which unset variable is picked next:

- `rarest` takes the **fewest** clauses holding `x` or `!x`, whichever count is
  larger; `glowest` the **most**. Only clauses that are not yet satisfied count
  (ties by id), recomputed after every assignment, so satisfying a clause removes
  that overlap from every variable still unset.
- `min-overlapping` takes `glowest`'s pick first, then at every step the unset
  variable with the smallest **(occurrences in satisfied clauses) / (total
  occurrences)** (ties by id). A variable that occurs nowhere has no ratio and
  goes last. The point is to keep picking variables mostly outside the clauses
  the first pick already satisfied, instead of following the same subsystem.

### Yanus

`--init yanus` changes the formula instead of the walk. Each attempt draws a
fresh `D ∈ [1,n]` and `R ∈ [1,n]` from the solver RNG, appends `D` variables,
and picks `R` random definitions `x = (±y_a) ∨ (±y_b)` (`x` an original
variable, `y_a,y_b` fresh, signs independent coin flips). With literals `l_a,l_b`
a definition is three clauses: `(¬l_a ∨ x)`, `(¬l_b ∨ x)` and
`(l_a ∨ l_b ∨ ¬x)`. Glowest runs on that expanded CNF; its values for the
original `n` variables are copied back and scored on the original formula.
The best start wins, and a zero-rogue mapped assignment ends the loop early.
`-timeout N` sets the attempt budget (default 8; `0` falls back to glowest).
`-saturate F` overrides the random `D` with `F · n_clauses` extra variables
(`F = 0` keeps the RNG choice); `R` stays random.
Both `plain` and `groups` mode dispatch to it.

### Gob

`--init gob` is a linear relaxation, not a clause walk. Before building the
relaxation, `no_twins` filters the input equation set: synchronous twins
(identical clauses) and asynchronous twins (the same literals with every sign
inverted) are reduced to their first occurrence. Literal order does not matter.
This filtering is local to Gob/Gob-sign/Gob-square/Gob-recult; the solver and the greedy
repair still use the complete original CNF. Each remaining clause becomes the
signed equation `sum(±x) = 1` (a positive literal is `+x`, a negative one `-x`),
and 64 cyclic Kaczmarz sweeps project the all-zero vector onto that filtered
system (one exact projection per remaining clause per sweep). Each value is then
shifted by the smallest non-negative amount, rounded to the nearest integer and
taken mod 2, which decodes the real vector to booleans. The shift is per
variable, not global, because a global shift cannot satisfy mixed-sign units:
`(x)` and `(-x)` always decode to the same parity.

The decode is only a starting point, so four greedy passes follow: a variable
whose bit satisfies more rogue clauses than it breaks is flipped, using the same
incremental counts as the search. Deterministic, no RNG.

### Gob-sign

`--init gob-sign` runs the same filter, Kaczmarz sweeps and greedy repair, but
decodes the real vector by sign: positive is true, zero or negative is false.

Sign, not magnitude, is the signal. The equation asks for **two** true literals
per clause, which a satisfying assignment usually does not give: on 20000/3.5,
43% of a solution's clauses have exactly one true literal (signed sum `-1`), and
15% have all three. So 58% of the equations are violated at the true solution,
its RMS residual is 1.52 -- worse than the Kaczmarz point's own 1.19. The
minimum of this system is not near SAT. What survives is the sign: it agrees with
a found solution 63.5% of the time, where the magnitude decode agrees only 52.5%.
Search from the sign decode alone solves 6/10 of the 20000/3.5 seeds at this
budget; the greedy repair brings gob-sign to 8/10, where gob solved 0/10.

#### Projection diagnostics

The projection is faithful in **count**: one equation per parsed clause.
`no_twins` drops 0 equations on random instances and 0–2.08% on AIM (all exact
duplicates, 0 asynchronous twins). The right-hand side is not the issue either:
`sign(v)` is invariant under scaling `b` by a positive constant, and a row with
`sum = 1 > 0` always has a true literal. The loss is joint: `m` rows share `n`
variables, and on every measured instance `rank(A) = n` while
`rank([A|b]) = n+1` (AIM residual of `b` against `col(A)` is 12–34). The system
is inconsistent, so cyclic Kaczmarz has no unique fixed point — each row order
settles at its own attractor, ~5 away from the least-squares solution. Two orders
of AIM 100-2_0 give 49/100 different signs, unchanged from 1000 to 4000 sweeps.

At 20000/3.5, permuting the clause order of the same 69,994 clauses flips
32.6–33.9% of the decoded signs (6,525–6,869 of 20,000 variables), while
`init_unsat` only moves within 869–952. That sign churn flips outcomes: seed 4
fails in file order but solves in 8/8 reorders; seed 1 solves in file order but
fails in 4/8. Crossing the two choices shows they are independent coin flips:

| seed | vary start only (same formula and order) | vary search order only (same start) |
|---|---|---|
| 4 | 5/8 solve | 5/8 solve |
| 10 | 7/8 solve | 6/8 solve |
| 1 | 8/8 solve | 7/8 solve |

Every instance above is satisfiable, so these misses are start/order artifacts,
not instance hardness.

Replacing Kaczmarz with exact rational arithmetic does not stabilize the signs.
`tools/exact_gob.py` solves `A x = 1` by Gauss-Jordan over `Q`, computes the unique
exact least-squares point `(A^T A)^-1 A^T 1`, and solves an exact full-rank
`n`-of-`m` subsystem for each row order. On AIM 100-3_4 and 100-6_0:

- the full system is **inconsistent** exactly (rank 100 vs augmented 101), so
  there is no symbolic solution to decode;
- the exact LS point is order-free, yet its sign agrees with a found SAT
  assignment only 48–51/100 times, with `|x|` median 0.08–0.17 — the magnitudes
  are compressed near zero;
- the exact square subsystem drifts 39–53/100 signs across row orders, and
  Kaczmarz drifts 43–58/100. Kaczmarz's signs differ from the exact LS point by
  38–58/100.

So the drift is not arithmetic error: exact arithmetic with a different set of
equations drifts just as much. Inconsistency leaves no canonical vector whose
sign could be stable.

Control (`--consistent`): with `b = A t` for a known SAT assignment `t ∈ {±1}`,
the system is consistent and has the unique solution `t` (`rank(A) = n`), so
**every** full-rank square subsystem must return `t`. It does: 100/100 sign
agreement, all `|x| = 1`, and **0** sign drift across the same row orders for
both Kaczmarz and the exact square solves. The drift is the inconsistency of
`b = 1`, not the arithmetic.

Reproduce (the generator mirrors `bench_rarest.c` exactly, including its parsed
clause-count drift):

```sh
make probe
mkdir -p /tmp/gobdiag
build/probe gen 20000 3.5 10 200000 9 /tmp/gobdiag
python3 tools/gen_cnf.py 100 3.5 1 > /tmp/gobdiag/small.cnf
python3 tools/projection_counts.py /tmp/gobdiag/small.cnf   # clauses, equations, ranks (small: SVD)
python3 tools/permute_clauses.py /tmp/gobdiag/b4.cnf /tmp/gobdiag/b4_p0.cnf 1004
build/probe run /tmp/gobdiag/b4_p0.cnf 200000 9            # outcome after reordering
build/probe start /tmp/gobdiag/b4.cnf /tmp/gobdiag/b4.txt
build/probe start /tmp/gobdiag/b4_p0.cnf /tmp/gobdiag/b4_p0.txt
python3 tools/projection_counts.py --signs /tmp/gobdiag/b4.txt /tmp/gobdiag/b4_p0.txt
build/probe xstart /tmp/gobdiag/b4_p0.cnf /tmp/gobdiag/b4.cnf 200000 9   # start only
build/probe xstart /tmp/gobdiag/b4.cnf /tmp/gobdiag/b4_p0.cnf 200000 9   # order only

# exact/symbolic projection on a small AIM instance (needs a known SAT assignment)
build/3sat <aim-100-3_4-yes1-1.cnf> --init glowest --steps 200000 --restarts 20 --out /tmp/gobdiag/truth.json
python3 tools/exact_gob.py <aim-100-3_4-yes1-1.cnf> /tmp/gobdiag/truth.json
python3 tools/exact_gob.py --consistent <aim-100-3_4-yes1-1.cnf> /tmp/gobdiag/truth.json
```

### Gob-square

`--init gob-square` runs the same idea in three phases:

1. **Filter and pick a square subsystem.** `no_twins` first removes repeated
   synchronous clauses and complementary asynchronous clauses. Of the remaining
   `m` equations, select `n` (one every `m/n` clauses), so the selected block has
   as many equations as variables. Here `m` is the post-filter equation count.
2. **Solve it.** 512 cyclic Kaczmarz sweeps over the selected rows only (the
   square system converges slower than the full one, hence the larger sweep
   count).
3. **Check the rest.** Every unselected filtered equation is evaluated on that solution.
   `GobSquareCheck` reports `selected_bad`, `other_bad` and `other_total`; the
   bench harness prints the averages as `gob-square phase 3`.

The solution is then decoded and repaired against the complete original CNF,
exactly like `gob-sign`. This is still a seed, not a solve: the selected rows can be linearly dependent (strided
selection makes no rank guarantee, e.g. `aim-50-3_4-yes1-1` is singular at
49/50), the values are fractional, and on the AIM corpus phase 3 finds almost
every unselected equation violated (`rest_bad` 30/30, 99/100, 200/200 on the
50-, 100- and 200-var rows checked). Reporting "no solution" is the expected
answer for `m > n`; the decode is what the search gets.

### Gob-recult

`--init gob-recult` (“recultivation”) augments each filtered clause equation
`L_i(x) = Σ(±x_v) = 1` with tail variables `Y_0..Y_{n-1}` on the right-hand
side:

`L_i(x) = 1 + Y_i + (-1)^(i+k) * Y_{(i+k) mod n} + Y_{(i + h) mod n}`,

where `n` is the number of filtered equations, `k` is chosen once per init from
the solver RNG and rounded to an even value, and `h = k/2` (indices modulo `n`).
`x` and `Y` (all init 0) are solved together by cyclic Kaczmarz projections
(`GOB_SWEEPS` passes) plus, each sweep, a coupling projection enforcing
`Σ_i Y_i = B`. By default `B` is found by brute-force around a center derived
from a BaddyMove partial assignment: with `b_i` the number of true literals in
clause `i`, the coupling identity gives `B(b) = Σ_i (−1)^i (b_i − 1)`. Every
integer `B` in `[B(b) − k·s, B(b) + k·s]`, `s = min(n/4+1, 128)`, is tried
(`-coef-k K`, default 2) and the decode with the fewest rogue clauses is kept.
The cap keeps init bounded on large instances (a 34k-clause ssa file took 37+ min
before it, 68s after). `-play-baseline P` instead sets `B = rng mod P`
(`P >= 1`, drawn from the solver RNG), and `-set-baseline N` fixes `B = N` (takes
precedence over `-play-baseline` if both are given); either explicit flag skips the
brute-force. Only `x` is kept: decoded by sign and repaired by `gob_repair`, like
`gob-sign`.

#### What the linear system can and cannot recover

The clause linearization is exact in 0/1 variables: `x ∨ y` is `x + y = b_i`,
`x ∨ ¬y` is `x − y + 1 = b_i`, where `b_i` is the number of true literals in the
clause (`1..3` for a satisfiable 3-CNF). In the ±1 code used here the RHS is
`2·b_i − 3 ∈ {−1, 1, 3}`; the constant `1` above corresponds to `b_i = 2`.

With the true `b` from a satisfying model, `A z = 2b − 3` recovers that model
**exactly** on all 17 hard AIM instances (`A` has full column rank `nv`, so the
solution is unique). So the information is entirely in the per-clause `b` vector.

A Kaczmarz state `x` is just `A⁺ r` for the current RHS `r`. Unless `r` is very
close to the true `2b−3`, that `x` is dominated by the CNF's own polarity
statistics: on AIM instances `corr(x_i, pos_i − neg_i) ≈ 0.76..0.82`, while
`corr(x_i, model_i) ≈ 0`, and `|x_i|` is *anti*-correlated with sign correctness.
The sign decode slips in the first ~12 sweeps and freezes at ~44–53% model
agreement; the magnitude/parity decodes are no better. Setting `B` to the exact
ideal `ΣY` (reconstructed from `M·Y = b−1`, values −6..19 here) still gives
UNKNOWN with 3–7 rogue clauses.

Since `M·Y = b − 1`, the baseline is a linear functional of the whole `b` vector:
`B = 1ᵀ M⁻¹ (b − 1)`. For this coupling matrix `1ᵀ M⁻¹ = ((−1)^i)` exactly (any
`k`, `n`), so `B(b) = Σ_i (−1)^i (b_i − 1)` — an O(n) closed form, verified against
a dense solve on all 17 AIM instances. Under BaddyMove's `b` it lands within
0–26 of the ideal `B*`, far tighter than `Σb` (240–600).

`B*` is the value that makes the *linear system* consistent, not the value that
decodes best: the best decode can sit 14–47 away from `B(b)` (e.g.
`100-1_6-yes1-1`: `B(b)=0`, unsat 3 at `B=0` versus 2 at `B=34`), because the
Kaczmarz → sign → repair pipeline is not monotone in `|B − B*|`. So `B(b)` is a
**search center**, not a substitute for the search. Centering on it with radius
`k·s` with `s = min(n/4+1, 128)` reproduces the old `[-k·Σb, k·Σb]` brute-force
exactly on all 17 AIM instances at default `k=2` (12/17 at `k=1`), in ~2.4× less
time (24.7s vs 58.2s).
The old range centered at zero needed radii up to 1200; `B(b)` puts the useful
window near the optimum.

Tried without gain: RHS `−1` (the "one true literal" encoding), magnitude-parity
decode, decoding from the full per-row corrected RHS (identical to decoding `x`),
200 Terminator rounds after each decode, iterative RHS projection, local search
over RHS with the exact solve, and using `B(b)` alone without a surrounding
search. Estimating `b_i` per clause is the SAT problem itself; calibration of `B`
and the decoder are not the bottleneck.

### Vice versa (`reverse-*`)

`--init reverse-rarest|reverse-glowest|reverse-min-overlapping` builds the
reversed CNF by negating every literal in every clause, runs the named
constructive start on that system, and maps the result back by negating the
assignment (`x = !x'`). The same bases are available as Likho pre-cooks.

### Terminator

`--init terminator` starts from a pre-cook assignment (majority by default;
configure with `-pre-cook`, including `neighborhood`/`neighborhood0`) and runs up
to `--terminator-rounds` (default 16) full-variable improvement passes. In each
pass, in the chosen `--terminator-order` (`forward`, `reverse`, or `random` — a
fresh RNG permutation each round), a variable is flipped if `make_count >
break_count` (strictly improving, counts re-evaluated per var). It stops early on
a no-flip pass or when solved. As an init it runs at restart 0, after which
normal search continues; gains typically saturate in a few rounds.

### Likho

`--init likho` starts from `-pre-cook` (`zero`, the default: every variable
`false`), then repairs rogue clauses. It only accepts moves that break no
satisfied clause, and each variable is flipped at most once:

- A **neutral** variable inside a rogue clause is flipped immediately; it breaks
  nothing and satisfies at least that clause.
- A **prime** is the sole support of some satisfied clause, so flipping it alone
  breaks that clause. Its **switch domain** is the set of other unflipped variables
  that must flip with it so every clause it breaks gains a new true literal. A
  repair may itself be a prime, so a domain chains: prime -> prime -> ... -> neutral.
  The domain is accepted only if the joint flip leaves every touched clause
  satisfied. A twin pair is the special case of a domain with one partner.
- Domains are recomputed after every firing, and the gold rule (every variable
  fires once) means two primes whose domains share a variable share that single
  flip, while a prime whose domain conflicts with an already fired one is simply
  not fired.
- A neutral flip can liberate a prime (the clause it was propping up gets another
  true literal); the next pass sees it as neutral and can flip it. By default
  domains are tried only when no neutral is left anywhere; `-deep-dive` reverses
  that order and tries a closed domain first.

A domain is capped at 8 variables and its closure search at 64 backtracking nodes,
so a prime with an unusually large or conflicting domain is skipped rather than
solved. `-deep-dive VARS NODES` raises or lowers both bounds. With the default
zero start the repair is deterministic for a given formula; `-pre-cook majority`
inherits the majority phase's RNG tie-breaking.

### Measurements

Measured on random 3-SAT, 10 seeds per row (3 seeds for the 5000-var row), 20
restarts, default solver seed. "old" is the previous total-occurrence rarest order,
kept for comparison; "likho-dd" is `-deep-dive 32 128`, "likho-pc" is
`-pre-cook majority`, "sign" is `--init gob-sign`, "sq" is `--init gob-square`.
All eleven starts share the same search code:

| instance | broken after init (maj / old / new / glow / mo / gob / sign / likho / dd / pc / sq) | solved | mean flips over solved (maj / old / new / glow / mo / gob / sign / likho / dd / pc / sq) |
|---|---|---|---|
| 1000 vars, ratio 2.0 | 78 / 15 / 28 / 4.5 / 12.7 / 14.2 / 4.0 / 0 / 0.2 / 0 / 13.6 | 10/10 each | 88 / 47 / 67 / 26 / 27 / 110 / 25 / 0 / 1 / 0 / 87 |
| 1000 vars, ratio 3.5 | 187 / 77 / 98 / 37 / 58 / 61 / 45 / 34 / 32 / 10 / 60 | 10/10 each | 11.1k / 16.1k / 12.0k / 19.4k / 7.2k / 14.1k / 6.5k / 18.1k / 17.6k / 9.8k / 18.7k |
| 20000 vars, ratio 3.5 | 3802 / 1527 / 2024 / 783 / 1213 / 1275 / 922 / 742 / 637 / 209 / 1240 | 9/10, 9/10, 9/10, 10/10, 8/10, 0/10, 8/10, 0/10, 0/10, 9/10, 0/10 | 123k / 169k / 162k / 129k / 143k / — / 168k / — / — / 129k / — |
| 5000 vars, ratio 3.9 | 1098 / 482 / 631 / 274 / 396 / 411 / 309 / 385 / 337 / 126 / 392 | 2/3 each | 3.06M / 0.76M / 1.84M / 4.40M / 4.40M / 1.88M / 3.37M / 4.40M / 4.40M / 5.68M / 4.40M |
| AIM yes-set, 48 instances | 46 / 18 / 21 / 16.7 / 19.0 / 17.2 / 16.9 / 23 / 22 / 22 / 16.4 | 27 / 31 / 31 / 31 / 31 / 31 / 31 / 31 / 31 / 27 / 31 | — |

`glowest` is the strongest of the three frequency starts. It leaves fewer broken
clauses than rarest-new on every random row (4.5 vs 28, 37 vs 98, 274 vs 631,
783 vs 2024) and at 20000/3.5 it is the only start that solves all 10 seeds (init
783, 129k flips, 0.9 s vs 2.4 s for majority). On the AIM yes-set it is 16.7
broken, just behind gob-square's 16.4, and solves the same 31 instances as rarest.
Taking the highest-overlap variables first pays off where rarest's
low-overlap-first pass leaves clauses hanging on a single literal.

`min-overlapping` sits between rarest-new and glowest on init (12.7 / 58 / 396 /
1213 / 19.0 on the same rows) and is the fastest frequency start at 1000/3.5
(7.2k flips vs glowest's 19.4k): after the glowest first pick it keeps choosing
variables mostly outside the solved part, so the search does not re-enter the same
subsystem. It does not scale as well, though: at 20000/3.5 it solves 8/10 where
glowest solves 10/10.

`gob` (magnitude decode) is the weakest of the two full-system starts. It leaves
14.2 broken at 1000/2.0 (vs gob-sign's 4.0), 61 at 1000/3.5 (vs 45), 1275 at
20000/3.5 (vs 922) and 411 at 5000/3.9 (vs 309), and at 20000/3.5 it solves
0/10 where gob-sign solves 8/10. Its flips are 2-4x higher on the 1000-var rows
(110 vs 25, 14.1k vs 6.5k) and lower at 5000/3.9 (1.88M vs 3.37M, on the two
seeds both solve). On the AIM yes-set it is 17.2 broken versus 16.9, and both
solve the same 31/48. The shift-and-parity decode is kept as the baseline the
sign decode is measured against.

`gob-sign` is the best non-Likho start at 1000/2.0 (4.0 broken vs glowest's 4.5;
25 vs 26 flips). At 1000/3.5 it starts higher than glowest (45 vs 37 broken) but
reaches SAT in a third of the flips (6.5k vs 19.4k). At 20000/3.5 it solves 8/10
(922 broken, 168k flips), where the magnitude decode solves 0/10. On the AIM
yes-set it is 16.9 broken, behind gob-square's 16.4 and glowest's 16.7, and
solves the same 31/48. At 5000/3.9 its 3.37M mean flips is behind both rarest
orders. The sign is what makes the relaxation usable; the equation itself (two
true literals per clause) is not a SAT objective.

`gob-square` shows what the square subsystem does and does not buy. Its phase-3
report says the solution does not satisfy the system: at 1000 vars ~89% of the
selected rows and every unselected row are violated, and at 20000 vars
17.8k of 20000 selected and 50.0k of 50.0k unselected. Kaczmarz has not converged
in 512 sweeps, and with `m > n` the full system has no exact solution anyway.
As a seed it is the best init on the AIM yes-set (16.4, ahead of glowest's 16.7)
and solves the same 31/48. On random 3-SAT it is marginally better than the
magnitude `gob` (13.6 vs 14.2, 60 vs 61, 1240 vs 1275, 392 vs 411 broken) but
clearly behind the sign `gob-sign` on every row (vs 4.0, 45, 922, 309), and at
20000/3.5 it solves 0/10 where gob-sign solves 8/10, for 8x the sweeps.

The zero start still leaves 3-5x fewer broken clauses than majority, and it solves
1000/2.0 outright at init (zero flips), but it is a much deeper local minimum. At
20000/3.5 it solves **0/10** at this budget, where majority and both rarest orders
solve 9/10 (glowest 10/10); fewer broken clauses does not mean an easier search.
`-pre-cook majority` restores the previous majority-start repair exactly (209
broken, 9/10, 129k flips), so pre-cooking is the escape from the zero-start basin.
On the AIM yes-set the zero start matches both rarest orders at 31/48 (majority and
the majority pre-cook stay at 27/48), so the constructive start pays off there.
`-deep-dive 32 128` trims the zero-start init count (637 at 20000, 337 at 5000) but
changes no solved count. From a majority pre-cook, domains-first makes 20000/3.5
much worse (1/10 at 32 128, 0/3 at the default bounds), so deep-dive is not the
escape route; pre-cook without it is. `majority` stays the default start for the
plain search.

The max-of-polarities rarest order leaves 2-3x fewer broken clauses than majority,
but ~30% more than the total-occurrence order. It is faster on 1000-var/3.5 and
20000-var/3.5, slower on 5000-var/3.9; on the AIM yes-set both rarest orders solve
the same 31/48 (identical failure sets) versus 27/48 for majority. It changes which
near-solutions are reachable more than it changes total coverage.

Rarest-first is still **worse** than majority at scale. Why: it satisfies each clause
with the first literal it can and never adds redundancy, so almost
every satisfied clause hangs on a single variable. On the 20000-var / ratio 3.5 instance
77% of variables are prime after rarest-first (62% after majority), only 23% are neutral
(38%), and 63% of the broken clauses contain no neutral variable at all, i.e. no free flip
(26% after majority) -- measured with the old ordering. Fewer problems, but most of the
remaining ones cannot be fixed
without breaking something. Likho attacks exactly that: a prime's switch domain
chains through other primes until it reaches a neutral, instead of giving up at the
first broken clause. Group mode's merge, which only fires neutrals,
is hit the same way: its leftover barely changed (e.g. 105.7 -> 95.0 unmerged at ratio
4.2).

### Coverage: instances no start can solve

The per-start counts above hide whether a failure is shared. Taking the union over
all eleven starts:

- **Random 3-SAT: 0 unsolved by all.** Every generated instance in the table was
  solved by at least one start, even where individual starts missed it (e.g. the
  zero-start Likho at 20000/3.5).
- **AIM yes-set: 17 of 48 unsolved by all** (31 covered). Every one is satisfiable,
  so these are budget failures at 100000 steps x 20 restarts, not UNSAT. By size:
  50-var `50-1_6-yes1-{1,2}` (2), 100-var `100-1_6-yes1-{1..4}` and
  `100-2_0-yes1-{1,2,4}` (7), 200-var `200-1_6-yes1-{1..4}` and
  `200-2_0-yes1-{1..4}` (8).
- The AIM union is exactly rarest's 31: `glowest`, `min-overlapping`, `gob`,
  `gob-sign`, `gob-square`, zero-start Likho and deep-dive each solve that identical set, so
  the extra starts add no new instance beyond rarest. Only `majority` and the majority pre-cook miss
  four of them.

Reproduce with `make bench-rarest ARGS="1000 3.5 10 200000 9"` (or
`ARGS="file.cnf 100000 20"`); `steps = 0` measures init quality only. The harness
also reimplements the old total-occurrence order as a baseline, so all eleven
starts run the same search code; its glowest row is `--init glowest`, its
min-overlapping row is `--init min-overlapping`, its gob row is `--init gob`, its
gob-sign row is `--init gob-sign`, its gob-square row is `--init gob-square`, its
deep-dive row uses `-deep-dive 32 128` and its pre-cook row uses `-pre-cook majority`.

## Group mode (`--mode groups`)

Sequential rogue groups with a neutral-driven merge, layered on the same solver
(`src/groups.c`). Ranking is unchanged: a variable is **neutral** iff flipping it
breaks no solved clause (`break_count == 0`), **prime** iff it is the sole
support of at least one (`break_count >= 1`). Neutrality belongs to the *current*
assignment, so it changes as flips happen. This invariant is pinned by a test.

```sh
build/3sat input.cnf --mode groups                      # defaults: 16 groups, free merge
build/3sat input.cnf --mode groups --max-groups 64 --merge directed --diversify 20
build/3sat input.cnf --mode groups --group-len 10 --max-groups 400   # at most 10 clauses per group
```

**Formation.** Start from the `--init` assignment `B` (majority by default).
1. `G1` = *all* clauses rogue under `B`. Solve `G1` on its own (a sub-formula that
   sees only its clauses, started from `B`) -> snapshot `X1`. The same variable may
   hold different polarities in different snapshots.
2. Apply `X1`, inspect the **whole** formula, collect *all* clauses that are rogue
   now into `G2`, solve independently -> `X2`. Earlier snapshots are kept.
3. Repeat until nothing is rogue (formation alone solved it), or the group cap,
   a repeated rogue set, or an unsolvable group stops it.

**Merge** (only if formation did not close the formula).
- Start from the snapshot with the most neutral variables (the first anchor).
- Each step picks the *source* group with the most fireable neutrals: zero breaks
  under the merged assignment, not fired yet, inside a still-rogue clause of that
  group, and the group's snapshot wants the opposite polarity (**directed** flip).
- When no directed flip is left, any other unfired neutral inside a rogue clause
  fires (**free** flip; `--merge directed` disables this tier).
- Every variable fires at most once for the whole merge. After each flip all
  statistics are re-derived, so primes can turn neutral and vice versa.
- A zero-break flip cannot un-solve a clause, so the rogue count falls with every
  flip: the merge always terminates, in at most `n_vars` flips.
- SAT is reported only if the merged assignment verifies against the whole
  formula. Otherwise: **UNKNOWN** plus a report of the merged part.

```json
{ "status": "UNKNOWN", "mode": "groups", "groups": 6, "group_sizes": [33, 13, 15, 13, 11, 11],
  "merged": true, "merge_flips": 10, "merge_flips_free": 7, "anchor_group": 1,
  "unmerged_clauses": [ {"clause": 514, "groups": [2, 4, 6]}, {"clause": 63, "groups": [2, 6]},
                        {"clause": 408, "groups": [2, 4, 6]} ],
  "neutral_vars": [6, 24, ...], "prime_vars": [1, 2, ...],
  "merged_assignment": [false, false, true, ...] }
```
Clause and variable ids in this report are 1-based (DIMACS style).
Each `unmerged_clauses` entry carries the group(s) that clause was rogue in. A clause that
keeps coming back in every second group (2, 4, 6 above) is the ping-pong between two groups
made visible; `"groups": []` means it never formed a group (possible with `--group-len`,
which leaves some rogue clauses waiting).
Build with `-DGROUPS_TRACE` to print every group as it forms.

**Group length (`--group-len N`, also `-group-len N`).** Caps the clauses per group.
`0` (default) keeps the original rule: a group is *all* currently rogue clauses. With
`N > 0` a group takes the `N` rogue clauses with the lowest ids and the rest wait for
later groups; formation still ends when nothing is rogue. `--max-groups` goes up to
1024 (each group stores a full snapshot, so memory is about groups x variables bytes).
Measured on 1000-variable instances (10 seeds per ratio, `--max-groups 400`):
smaller groups trimmed the leftover a little (ratio 3.0: 34.7 unmerged clauses with
`all`, 30.8 with `1`) but solved nothing extra, and with `N <= 3` formation stops after
~15-28 groups because the same lowest-id clauses keep coming back (repeat detection).

**What to expect (measured, random 3-SAT).** Independent solving cannot see the
clauses outside its group, so two groups that share variables undo each other
(each fixes ~6-11 clauses and re-breaks about as many). On 1000-variable instances
(`--max-groups 64 --steps 100000 --restarts 3`, 10 seeds per ratio), the plain solver found a solution in 10/10 runs at ratios
2.0-3.4 and group mode in 0/10; `--diversify 20` (re-solve a repeated rogue set
with random picks) raises ratio 2.0 to 8/10 and changes nothing from ratio 3.0 up.
On small instances (20-200 variables, ratio 2-3, cap 64) formation closes alone in
~26% of runs and the merge completes another ~15%; the rest end with a few
unmerged clauses, which is the diagnostic output. Group mode is a decomposition and
reporting tool, not a faster solver. The plain solver stays the default.

## Differences from the Rust version

- Structural UNSAT: a unit clause and its negation are detected before search and
  returned as UNSAT with the conflicting clause pairs. The Rust version reports
  UNKNOWN.
- The DIMACS `%` end-of-file marker (used by SATLIB files) is accepted; the Rust
  parser rejects it.
- If the very last allowed flip satisfies the formula, C reports SAT. The Rust
  loop only checks at the top of a step, so it could report UNKNOWN with 0
  unsatisfied clauses.
- `make_count` equals `rogue_occurrences` (a rogue clause has no true literal),
  so both come from the same pass.
- Extra options: `--strategy`, `--progress`, `--mode`, `--max-groups`, `--merge`, `--diversify`,
  `--group-len` (also accepted as `-group-len`), `--init`, `--terminator-rounds`,
  `--terminator-order`, `--neighborhood-timeout`, `--neighborhood-start`,
  `-deep-dive`, `-pre-cook`, `-play-baseline`, `-set-baseline`, `-coef-k`,
  `-timeout`, `-saturate`.

## Layout

```
src/result.h   Result/Error/TRY macros
src/sat.h      public API
src/sat.c      DIMACS parser, strategies, solver
src/groups.h   group formation + merge API (and the algorithm spec)
src/groups.c   sequential rogue groups, dynamic-anchor neutral merge
src/main.c     CLI
tests/         test_sat.c + fixtures
tools/gen_cnf.py             random 3-CNF generator
tools/bench_rarest.c         init + search benchmark (make bench-rarest)
tools/probe.c                gob-sign projection diagnostics (make probe)
tools/permute_clauses.py     shuffle clause order of a DIMACS file
tools/projection_counts.py   equations/ranks and decoded-sign comparison
tools/exact_gob.py           exact rational projection (Kaczmarz-free) diagnostics
tools/run_satlib.py          run 3sat over a CNF corpus and summarize by subdir
tools/cmp_aim17_hard.py      compare inits (and optional DPLL) on the 17 hard AIM-yes cases
tools/dpll_solve.c           naive DPLL (UP + backtracking) reference solver, DIMACS in/out
```
