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

Restart 0 starts from `--init majority` (default: every variable takes its majority
polarity) or `--init rarest`, a deterministic constructive start (no RNG, works in both
`plain` and `groups` mode; later restarts stay random):

1. Repeatedly set the currently **rarest** unassigned variable: the fewest clauses
   holding `x` or `!x`, whichever count is larger, and only clauses that are not yet
   satisfied count (ties by id). The counts are recomputed after every assignment, so
   satisfying a clause removes that overlap from every variable still unset.
2. The variable takes the value that satisfies more **forced**
   clauses (open clauses where it is the last unset variable); else more **open**
   (not yet satisfied) clauses; else the larger total polarity; else `false`.

Measured on random 3-SAT, 10 seeds per row (3 seeds for the 5000-var row), 20
restarts. "old" is the previous total-occurrence order, kept for comparison:

| instance | broken after init (majority / old / new) | solved (majority / old / new) | mean flips (majority / old / new) |
|---|---|---|---|
| 1000 vars, ratio 2.0 | 76 / 15 / 27 | 10/10, 10/10, 10/10 | 85 / 32 / 42 |
| 1000 vars, ratio 3.5 | 187 / 77 / 98 | 10/10, 10/10, 10/10 | 11.1k / 16.1k / 12.0k |
| 20000 vars, ratio 3.5 | 3802 / 1527 / 2024 | 9/10, 9/10, 9/10 | 123k / 169k / 162k |
| 5000 vars, ratio 3.9 | 1098 / 482 / 631 | 2/3, 2/3, 2/3 | 3.06M / 0.76M / 1.84M |
| AIM yes-set, 48 instances | 44 / 18 / 21 | 27 / 31 / 31 | — |

The max-of-polarities order leaves 2-3x fewer broken clauses than majority, but
~30% more than the total-occurrence order. It is faster on 1000-var/3.5 and
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
without breaking something. Group mode's merge, which only fires neutrals, is hit the same
way: its leftover barely changed (e.g. 105.7 -> 95.0 unmerged at ratio 4.2).
`majority` stays the default.

Reproduce with `make bench-rarest ARGS="1000 3.5 10 200000 9"` (or
`ARGS="file.cnf 100000 20"`); `steps = 0` measures init quality only. The harness
also reimplements the old total-occurrence order as a baseline, so all three starts
run the same search code.

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

**Formation.** Start from the majority-polarity assignment `B`.
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
