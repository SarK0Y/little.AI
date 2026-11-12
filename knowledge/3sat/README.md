# context-3sat

A small, dependency-free Rust 3-SAT local-search experiment for `little.AI` context handling.

The implementation keeps the useful core of the earlier variable-centric design:

- variables have only variable states (`Unused`, `Prime`, or `Neutral`);
- clauses alone have clause states (`Solved` or `Rogue`);
- variable statistics may count incidences in rogue clauses, but a variable is never itself rogue;
- clauses maintain an incremental count of currently true literals;
- a variable is **prime** when it is the sole satisfying literal in at least one satisfied clause;
- a variable is **neutral** when it is used but is not prime; it may occur in rogue clauses too;
- neutral variables carry the minimum support count among the clauses they satisfy;
- candidate variables are re-ranked from the current assignment at every search step;
- flipping a variable updates only clauses containing that variable, then re-ranks variables appearing in those affected clauses.

## Build

```sh
cargo test --manifest-path knowledge/3sat/Cargo.toml
cargo build --release --manifest-path knowledge/3sat/Cargo.toml
```

No external crates are required.

## Run

```sh
cargo run --release --manifest-path knowledge/3sat/Cargo.toml -- path/to/input.cnf
cargo run --release --manifest-path knowledge/3sat/Cargo.toml -- path/to/input.cnf --steps 250000 --restarts 40 --noise 0.20 --seed 42
```

Default output is written to `./knowledge/3sat/results/latest.json` relative to the current working directory. Override it with `--out FILE`.

Exit codes:

- `0`: SAT; the returned assignment is independently verified against the input.
- `1`: input, output, or internal verification error.
- `2`: command-line usage error.
- `3`: search budget exhausted; result is UNKNOWN.

**Important:** this first engine is stochastic local search. It can find and verify SAT assignments, but it cannot prove UNSAT. It reports `UNKNOWN` when the search budget expires. Do not interpret UNKNOWN as UNSAT.

## Search cycle

1. Select a currently unsatisfied (rogue) clause.
2. Recalculate the candidate variables' current break/make costs.
3. Re-rank candidates: neutral before prime, then stronger minimum support, fewer broken clauses, and more newly satisfied clauses.
4. With probability `noise`, choose a random variable from the rogue clause; otherwise choose the top-ranked candidate.
5. Flip it, incrementally update affected clause counters and the rogue-clause index, and re-rank variable states for the next step.

The initial assignment uses majority polarity, matching the spirit of the old naive polarity ranking. Subsequent restarts use randomized assignments.

## Current boundaries

- DIMACS CNF input is supported, including comments and clauses spanning lines.
- Clauses of length 1–3 are strict 3-SAT input. Longer clauses are accepted as general CNF and trigger a warning.
- Duplicate literals are removed and tautological clauses are discarded.
- This is a baseline for experimentation, not yet a complete DPLL/CDCL solver.
