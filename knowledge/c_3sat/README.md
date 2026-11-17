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

Restart 0 starts from one of six starts (later restarts are always random):

- `--init majority` (default): every variable takes its majority polarity.
- `--init rarest`: deterministic constructive start (no RNG), rarest variables first.
- `--init glowest`: deterministic constructive start, most frequent variables first.
- `--init min-overlapping`: deterministic constructive start, first the most
  frequent variable, then the one overlapping the solved part least.
- `--init gob`: deterministic linear-relaxation start (no RNG), clauses as signed
  equations solved by Kaczmarz sweeps and decoded mod 2.
- `--init likho`: all false, then a neutral/switch-domain repair pass.
- `-deep-dive VARS NODES`: implies `--init likho`, caps switch domains at VARS
  variables, gives each prime at most NODES search nodes, and fires domains
  before neutral repairs.
- `-pre-cook zero|majority|rarest|glowest|min-overlapping|gob`: implies `--init likho`;
  phase 1 is that assignment instead of the default all-false one (a partially
  solved input).

The six starts work in both `plain` and `groups` mode; `-deep-dive` is a variant
of `likho`.

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

### Gob

`--init gob` is a linear relaxation, not a clause walk. Each clause becomes the
signed equation `sum(±x) = 1` (a positive literal is `+x`, a negative one `-x`),
and 64 cyclic Kaczmarz sweeps project the all-zero vector onto that system (one
exact projection per clause per sweep). Each value is then shifted by the
smallest non-negative amount, rounded to the nearest integer and taken mod 2,
which decodes the real vector to booleans. The shift is per variable, not global,
because a global shift cannot satisfy mixed-sign units: `(x)` and `(-x)` always
decode to the same parity.

The decode is only a starting point, so four greedy passes follow: a variable
whose bit satisfies more rogue clauses than it breaks is flipped, using the same
incremental counts as the search. Deterministic, no RNG.

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
`-pre-cook majority`. All nine starts share the same search code:

| instance | broken after init (maj / old / new / glow / mo / gob / likho / dd / pc) | solved | mean flips over solved (maj / old / new / glow / mo / gob / likho / dd / pc) |
|---|---|---|---|
| 1000 vars, ratio 2.0 | 78 / 15 / 28 / 4.5 / 12.7 / 14.2 / 0 / 0.2 / 0 | 10/10 each | 88 / 47 / 67 / 26 / 27 / 110 / 0 / 1 / 0 |
| 1000 vars, ratio 3.5 | 187 / 77 / 98 / 37 / 58 / 61 / 34 / 32 / 10 | 10/10 each | 11.1k / 16.1k / 12.0k / 19.4k / 7.2k / 14.1k / 18.1k / 17.6k / 9.8k |
| 20000 vars, ratio 3.5 | 3802 / 1527 / 2024 / 783 / 1213 / 1275 / 742 / 637 / 209 | 9/10, 9/10, 9/10, 10/10, 8/10, 0/10, 0/10, 0/10, 9/10 | 123k / 169k / 162k / 129k / 143k / — / — / — / 129k |
| 5000 vars, ratio 3.9 | 1098 / 482 / 631 / 274 / 396 / 411 / 385 / 337 / 126 | 2/3 each | 3.06M / 0.76M / 1.84M / 4.40M / 4.40M / 1.88M / 4.40M / 4.40M / 5.68M |
| AIM yes-set, 48 instances | 46 / 18 / 21 / 16.7 / 19.0 / 17.2 / 23 / 22 / 22 | 27 / 31 / 31 / 31 / 31 / 31 / 31 / 31 / 27 | — |

`glowest` is the best init here. It leaves fewer broken clauses than rarest-new on
every random row (4.5 vs 28, 37 vs 98, 274 vs 631, 783 vs 2024) and at 20000/3.5 it
is the only start that solves all 10 seeds (init 783, 129k flips, 0.9 s vs 2.4 s for
majority). On the AIM yes-set it has the lowest init count (16.7) and solves the same
31 instances as rarest. Taking the highest-overlap variables first pays off where
rarest's low-overlap-first pass leaves clauses hanging on a single literal.

`min-overlapping` sits between rarest-new and glowest on init (12.7 / 58 / 396 /
1213 / 19.0 on the same rows) and is the fastest frequency start at 1000/3.5
(7.2k flips vs glowest's 19.4k): after the glowest first pick it keeps choosing
variables mostly outside the solved part, so the search does not re-enter the same
subsystem. It does not scale as well, though: at 20000/3.5 it solves 8/10 where
glowest solves 10/10.

`gob` is competitive at 1000 vars (init 14.2 / 61, next to min-overlapping's
12.7 / 58; 110 / 14.1k flips) and on the AIM yes-set (17.2 broken, second only to
glowest's 16.7, and the same 31/48). At 5000/3.9 its 1.88M mean flips is second
only to rarest-old, but at 20000/3.5 it solves 0/10 like the zero start: 1275
broken clauses still sit in a deep basin. The linear relaxation finds a good
decode, not a good basin.

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
all nine starts:

- **Random 3-SAT: 0 unsolved by all.** Every generated instance in the table was
  solved by at least one start, even where individual starts missed it (e.g. the
  zero-start Likho at 20000/3.5).
- **AIM yes-set: 17 of 48 unsolved by all** (31 covered). Every one is satisfiable,
  so these are budget failures at 100000 steps x 20 restarts, not UNSAT. By size:
  50-var `50-1_6-yes1-{1,2}` (2), 100-var `100-1_6-yes1-{1..4}` and
  `100-2_0-yes1-{1,2,4}` (7), 200-var `200-1_6-yes1-{1..4}` and
  `200-2_0-yes1-{1..4}` (8).
- The AIM union is exactly rarest's 31: `glowest`, `min-overlapping`, `gob`,
  zero-start Likho and deep-dive each solve that identical set, so the extra starts
  add no new instance beyond rarest. Only `majority` and the majority pre-cook miss
  four of them.

Reproduce with `make bench-rarest ARGS="1000 3.5 10 200000 9"` (or
`ARGS="file.cnf 100000 20"`); `steps = 0` measures init quality only. The harness
also reimplements the old total-occurrence order as a baseline, so all nine starts
run the same search code; its glowest row is `--init glowest`, its min-overlapping
row is `--init min-overlapping`, its gob row is `--init gob`, its deep-dive row
uses `-deep-dive 32 128` and its pre-cook row uses `-pre-cook majority`.

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
  `--group-len` (also accepted as `-group-len`), `--init`.

## Layout

```
src/result.h   Result/Error/TRY macros
src/sat.h      public API
src/sat.c      DIMACS parser, strategies, solver
src/groups.h   group formation + merge API (and the algorithm spec)
src/groups.c   sequential rogue groups, dynamic-anchor neutral merge
src/main.c     CLI
tests/         test_sat.c + fixtures
tools/gen_cnf.py     random 3-CNF generator
tools/bench_rarest.c init + search benchmark (make bench-rarest)
```
