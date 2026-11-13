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
- Extra options: `--strategy`, `--progress`.

## Layout

```
src/result.h   Result/Error/TRY macros
src/sat.h      public API
src/sat.c      DIMACS parser, strategies, solver
src/main.c     CLI
tests/         test_sat.c + fixtures
tools/gen_cnf.py   random 3-CNF generator
```
