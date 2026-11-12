//! Variable-centric 3-SAT search with incremental clause counters.
//!
//! This is a stochastic local-search solver: it returns SAT when it finds a
//! satisfying assignment, and UNKNOWN when its configured search budget ends.
//! It deliberately never reports UNSAT (local search cannot prove it).

use std::collections::HashSet;
use std::error::Error;
use std::fmt;
use std::fs;
use std::path::Path;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Lit {
    pub var: usize,
    pub positive: bool,
}

impl Lit {
    #[inline]
    pub fn value(self, assignment: bool) -> bool {
        assignment == self.positive
    }
}

#[derive(Clone, Debug)]
pub struct Cnf {
    pub n_vars: usize,
    pub clauses: Vec<Vec<Lit>>,
}

#[derive(Debug)]
pub struct ParseError(pub String);
impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result { f.write_str(&self.0) }
}
impl Error for ParseError {}

impl Cnf {
    pub fn from_dimacs_str(input: &str) -> Result<Self, ParseError> {
        let mut declared_vars = None;
        let mut declared_clauses = None;
        let mut tokens: Vec<i64> = Vec::new();
        for (line_no, line) in input.lines().enumerate() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('c') { continue; }
            if line.starts_with('p') {
                let fields: Vec<_> = line.split_whitespace().collect();
                if fields.len() != 4 || fields[1] != "cnf" {
                    return Err(ParseError(format!("line {}: expected `p cnf VARS CLAUSES`", line_no + 1)));
                }
                declared_vars = Some(fields[2].parse::<usize>().map_err(|_| ParseError(format!("line {}: invalid variable count", line_no + 1)))?);
                declared_clauses = Some(fields[3].parse::<usize>().map_err(|_| ParseError(format!("line {}: invalid clause count", line_no + 1)))?);
                continue;
            }
            for field in line.split_whitespace() {
                let value = field.parse::<i64>().map_err(|_| ParseError(format!("line {}: invalid DIMACS token `{field}`", line_no + 1)))?;
                tokens.push(value);
            }
        }
        let n_vars = declared_vars.ok_or_else(|| ParseError("missing `p cnf` header".into()))?;
        let expected_clauses = declared_clauses.unwrap();
        let mut clauses = Vec::new();
        let mut current = Vec::new();
        for value in tokens {
            if value == 0 {
                // Remove duplicate literals. Tautologies can never constrain a model.
                let mut seen = HashSet::new();
                let mut normalized = Vec::new();
                let mut tautology = false;
                for lit in current.drain(..) {
                    let key = (lit.var, lit.positive);
                    if seen.contains(&(lit.var, !lit.positive)) { tautology = true; break; }
                    if seen.insert(key) { normalized.push(lit); }
                }
                if !tautology { clauses.push(normalized); }
            } else {
                let abs = value.checked_abs().ok_or_else(|| ParseError("literal integer overflow".into()))? as usize;
                if abs == 0 || abs > n_vars {
                    return Err(ParseError(format!("literal {value} is outside 1..={n_vars}")));
                }
                current.push(Lit { var: abs - 1, positive: value > 0 });
            }
        }
        if !current.is_empty() { return Err(ParseError("unterminated clause: missing 0".into())); }
        // Tautologies are omitted, so compare declared count before normalization only
        // when no tautologies were present. DIMACS generators sometimes include them.
        if clauses.len() > expected_clauses {
            return Err(ParseError(format!("header declares {expected_clauses} clauses, parsed {}", clauses.len())));
        }
        Ok(Self { n_vars, clauses })
    }

    pub fn from_dimacs_file(path: impl AsRef<Path>) -> Result<Self, Box<dyn Error>> {
        Ok(Self::from_dimacs_str(&fs::read_to_string(path)?)?)
    }

    pub fn is_3sat(&self) -> bool { self.clauses.iter().all(|c| c.len() <= 3) }
}

/// Variable classification. Rogue is deliberately NOT a variable state:
/// only clauses can be rogue.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum VarState {
    Unused,
    Prime,
    Neutral { min_support: usize },
}

/// Satisfaction state belongs to a clause, never to a variable.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ClauseState {
    Rogue,
    Solved,
}

#[derive(Clone, Copy, Debug)]
pub struct ClauseStats {
    /// Number of currently true literals in this clause.
    pub satisfying_literals: usize,
    pub state: ClauseState,
}

impl ClauseStats {
    fn new(satisfying_literals: usize) -> Self {
        Self {
            satisfying_literals,
            state: if satisfying_literals == 0 { ClauseState::Rogue } else { ClauseState::Solved },
        }
    }

    fn set_satisfying_literals(&mut self, count: usize) {
        self.satisfying_literals = count;
        self.state = if count == 0 { ClauseState::Rogue } else { ClauseState::Solved };
    }
}

#[derive(Clone, Debug)]
pub struct VarStats {
    pub state: VarState,
    /// Number of clauses in which this variable's literal is currently true.
    pub satisfied_clause_occurrences: usize,
    /// Number of rogue clauses containing this variable. This is an incidence
    /// count only; it does not make the variable rogue.
    pub rogue_clause_occurrences: usize,
    pub break_count: usize,
    pub make_count: usize,
    pub rank: usize,
}

#[derive(Clone, Debug)]
pub struct SolverConfig {
    pub max_steps: usize,
    pub restarts: usize,
    /// Probability of choosing a random variable from the selected rogue clause.
    pub noise: f64,
    pub seed: u64,
}
impl Default for SolverConfig {
    fn default() -> Self {
        Self { max_steps: 100_000, restarts: 20, noise: 0.22, seed: 0x9e37_79b9_7f4a_7c15 }
    }
}

#[derive(Clone, Debug)]
pub enum SolveResult {
    Sat { assignment: Vec<bool>, steps: usize, restart: usize },
    Unknown { steps: usize, best_unsatisfied: usize },
}

#[derive(Clone, Debug)]
struct Occurrence { clause: usize, positive: bool }

/// Incremental state. A flip touches only clauses containing that variable.
pub struct Solver {
    cnf: Cnf,
    assignment: Vec<bool>,
    clauses: Vec<ClauseStats>,
    occurrences: Vec<Vec<Occurrence>>,
    /// Clause IDs currently in the Rogue state; this is an index for fast selection.
    rogue_clauses: Vec<usize>,
    rogue_clause_pos: Vec<usize>,
    stats: Vec<VarStats>,
    rng: XorShift64,
}

impl Solver {
    pub fn new(cnf: Cnf, seed: u64) -> Self {
        let mut occurrences = vec![Vec::new(); cnf.n_vars];
        for (ci, clause) in cnf.clauses.iter().enumerate() {
            for lit in clause { occurrences[lit.var].push(Occurrence { clause: ci, positive: lit.positive }); }
        }
        let n = cnf.clauses.len();
        let mut solver = Self {
            assignment: vec![false; cnf.n_vars], clauses: vec![ClauseStats::new(0); n],
            occurrences, rogue_clauses: Vec::new(), rogue_clause_pos: vec![usize::MAX; n],
            stats: vec![VarStats { state: VarState::Unused, satisfied_clause_occurrences: 0, rogue_clause_occurrences: 0, break_count: 0, make_count: 0, rank: usize::MAX }; cnf.n_vars],
            cnf, rng: XorShift64::new(seed),
        };
        solver.rebuild_counts();
        solver
    }

    pub fn assignment(&self) -> &[bool] { &self.assignment }
    pub fn variable_stats(&self) -> &[VarStats] { &self.stats }
    pub fn unsatisfied_count(&self) -> usize { self.rogue_clauses.len() }
    pub fn clause_stats(&self) -> &[ClauseStats] { &self.clauses }

    fn rebuild_counts(&mut self) {
        for (ci, clause) in self.cnf.clauses.iter().enumerate() {
            let count = clause.iter().filter(|l| l.value(self.assignment[l.var])).count();
            self.clauses[ci].set_satisfying_literals(count);
        }
        self.rogue_clauses.clear();
        self.rogue_clause_pos.fill(usize::MAX);
        for ci in 0..self.clauses.len() {
            if self.clauses[ci].state == ClauseState::Rogue { self.add_rogue_clause(ci); }
        }
        self.rerank_all();
    }

    fn add_rogue_clause(&mut self, clause: usize) {
        if self.rogue_clause_pos[clause] == usize::MAX {
            self.rogue_clause_pos[clause] = self.rogue_clauses.len();
            self.rogue_clauses.push(clause);
        }
    }
    fn remove_rogue_clause(&mut self, clause: usize) {
        let pos = self.rogue_clause_pos[clause];
        if pos == usize::MAX { return; }
        let last = self.rogue_clauses.pop().unwrap();
        if pos < self.rogue_clauses.len() { self.rogue_clauses[pos] = last; self.rogue_clause_pos[last] = pos; }
        self.rogue_clause_pos[clause] = usize::MAX;
    }

    fn rerank_all(&mut self) {
        let vars: Vec<usize> = (0..self.cnf.n_vars).collect();
        self.rerank_vars(&vars);
    }

    fn rerank_vars(&mut self, vars: &[usize]) {
        // Only variables in clauses touched by the last flip need re-ranking.
        // Their status is always derived from the current assignment.
        for &v in vars {
            let mut satisfied_occurrences = 0;
            let mut rogue_clause_occurrences = 0;
            let mut min_support = usize::MAX;
            let mut prime = false;
            for occ in &self.occurrences[v] {
                if self.clauses[occ.clause].state == ClauseState::Rogue {
                    rogue_clause_occurrences += 1;
                    continue;
                }
                let literal_is_true = self.assignment[v] == occ.positive;
                if literal_is_true {
                    satisfied_occurrences += 1;
                    let support = self.clauses[occ.clause].satisfying_literals;
                    min_support = min_support.min(support);
                    if support == 1 { prime = true; }
                }
            }
            let state = if self.occurrences[v].is_empty() { VarState::Unused }
                else if prime { VarState::Prime }
                else if satisfied_occurrences == 0 { VarState::Neutral { min_support: 0 } }
                else { VarState::Neutral { min_support } };
            self.stats[v] = VarStats { state,
                satisfied_clause_occurrences: satisfied_occurrences,
                rogue_clause_occurrences,
                break_count: 0, make_count: 0, rank: usize::MAX };
        }
    }

    fn flip(&mut self, var: usize) {
        self.assignment[var] = !self.assignment[var];
        // Every variable in an affected clause may have changed support class.
        let mut touched_vars = Vec::new();
        // A variable occurs at most once per normalized clause.
        for oi in 0..self.occurrences[var].len() {
            let clause = self.occurrences[var][oi].clause;
            let old = self.clauses[clause].satisfying_literals;
            let lit_becomes_true = self.assignment[var] == self.occurrences[var][oi].positive;
            let new = if lit_becomes_true { old + 1 } else { old - 1 };
            self.clauses[clause].set_satisfying_literals(new);
            if old == 0 && new > 0 { self.remove_rogue_clause(clause); }
            else if old > 0 && new == 0 { self.add_rogue_clause(clause); }
            for lit in &self.cnf.clauses[clause] {
                if !touched_vars.contains(&lit.var) { touched_vars.push(lit.var); }
            }
        }
        self.rerank_vars(&touched_vars);
    }

    fn flip_costs(&self, var: usize) -> (usize, usize) {
        let mut breaks = 0;
        let mut makes = 0;
        for occ in &self.occurrences[var] {
            let count = self.clauses[occ.clause].satisfying_literals;
            let currently_true = self.assignment[var] == occ.positive;
            if currently_true && count == 1 { breaks += 1; }
            if !currently_true && count == 0 { makes += 1; }
        }
        (breaks, makes)
    }

    fn rank_candidates(&mut self, clause_id: usize) -> Vec<usize> {
        let clause = &self.cnf.clauses[clause_id];
        let mut candidates: Vec<(usize, u8, usize, usize, usize)> = Vec::new();
        for lit in clause {
            let v = lit.var;
            if candidates.iter().any(|x| x.0 == v) { continue; }
            let (breaks, makes) = self.flip_costs(v);
            self.stats[v].break_count = breaks;
            self.stats[v].make_count = makes;
            let (class, support) = match self.stats[v].state {
                VarState::Neutral { min_support } => (0, min_support),
                VarState::Prime => (1, 1),
                VarState::Unused => (2, usize::MAX),
            };
            // Neutral variables are preferred; among them, prefer safer support,
            // fewer breaks, then more makes. Recomputed for each decision.
            candidates.push((v, class, support, breaks, makes));
        }
        candidates.sort_by(|a, b| {
            a.1.cmp(&b.1)
                .then_with(|| b.2.cmp(&a.2))
                .then_with(|| a.3.cmp(&b.3))
                .then_with(|| b.4.cmp(&a.4))
                .then_with(|| a.0.cmp(&b.0))
        });
        for (rank, item) in candidates.iter().enumerate() { self.stats[item.0].rank = rank; }
        candidates.into_iter().map(|x| x.0).collect()
    }

    fn random_assignment(&mut self, majority: bool) {
        for v in 0..self.cnf.n_vars {
            let positive = self.occurrences[v].iter().filter(|o| o.positive).count();
            let negative = self.occurrences[v].len().saturating_sub(positive);
            self.assignment[v] = if majority && positive != negative { positive > negative } else { self.rng.next_bool() };
        }
        self.rebuild_counts();
    }

    pub fn solve(&mut self, config: &SolverConfig) -> SolveResult {
        if self.cnf.clauses.iter().any(|c| c.is_empty()) {
            return SolveResult::Unknown { steps: 0, best_unsatisfied: self.cnf.clauses.len() };
        }
        if self.cnf.clauses.is_empty() { return SolveResult::Sat { assignment: vec![false; self.cnf.n_vars], steps: 0, restart: 0 }; }
        let mut total_steps = 0;
        let mut best = usize::MAX;
        for restart in 0..=config.restarts {
            self.random_assignment(restart == 0);
            for _ in 0..config.max_steps {
                if self.rogue_clauses.is_empty() {
                    return SolveResult::Sat { assignment: self.assignment.clone(), steps: total_steps, restart };
                }
                best = best.min(self.rogue_clauses.len());
                let rogue_pick = self.rng.index(self.rogue_clauses.len());
                let clause_id = self.rogue_clauses[rogue_pick];
                let candidates = self.rank_candidates(clause_id);
                if candidates.is_empty() { break; }
                let neutral_count = candidates.iter().take_while(|&&v| matches!(self.stats[v].state, VarState::Neutral { .. })).count();
                let random_pool = if neutral_count > 0 { neutral_count } else { candidates.len() };
                let chosen = if self.rng.next_f64() < config.noise {
                    candidates[self.rng.index(random_pool)]
                } else {
                    candidates[0]
                };
                self.flip(chosen);
                total_steps += 1;
            }
        }
        SolveResult::Unknown { steps: total_steps, best_unsatisfied: best.min(self.rogue_clauses.len()) }
    }
}

#[derive(Clone, Debug)]
struct XorShift64 { state: u64 }
impl XorShift64 {
    fn new(seed: u64) -> Self { Self { state: if seed == 0 { 0xa076_1d64_78bd_642f } else { seed } } }
    fn next_u64(&mut self) -> u64 {
        let mut x = self.state;
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        self.state = x; x
    }
    fn next_bool(&mut self) -> bool { self.next_u64() & 1 != 0 }
    fn next_f64(&mut self) -> f64 { (self.next_u64() >> 11) as f64 / ((1u64 << 53) as f64) }
    fn index(&mut self, upper: usize) -> usize { (self.next_u64() as usize) % upper }
}

pub fn verify(cnf: &Cnf, assignment: &[bool]) -> bool {
    assignment.len() >= cnf.n_vars && cnf.clauses.iter().all(|clause|
        clause.iter().any(|lit| lit.value(assignment[lit.var])))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn dimacs_multiline_and_comments() {
        let cnf = Cnf::from_dimacs_str("c demo\np cnf 3 2\n1 -2\n0\n3 0\n").unwrap();
        assert_eq!(cnf.n_vars, 3);
        assert_eq!(cnf.clauses.len(), 2);
    }
    #[test]
    fn verifies_or_inside_and_between_clauses() {
        let cnf = Cnf::from_dimacs_str("p cnf 2 2\n1 2 0\n-1 2 0\n").unwrap();
        assert!(verify(&cnf, &[false, true]));
        assert!(!verify(&cnf, &[true, false]));
    }
    #[test]
    fn solves_simple_3sat() {
        let cnf = Cnf::from_dimacs_str("p cnf 3 3\n1 2 3 0\n-1 2 0\n-2 3 0\n").unwrap();
        let mut solver = Solver::new(cnf.clone(), 42);
        let result = solver.solve(&SolverConfig { max_steps: 1000, restarts: 5, noise: 0.2, seed: 42 });
        match result { SolveResult::Sat { assignment, .. } => assert!(verify(&cnf, &assignment)), _ => panic!("expected SAT") }
    }
    #[test]
    fn empty_clause_does_not_claim_sat() {
        let cnf = Cnf::from_dimacs_str("p cnf 1 1\n0\n").unwrap();
        let mut solver = Solver::new(cnf, 1);
        assert!(matches!(solver.solve(&SolverConfig::default()), SolveResult::Unknown { .. }));
    }

    #[test]
    fn rogue_is_a_clause_state_not_a_variable_state() {
        let cnf = Cnf::from_dimacs_str("p cnf 2 2\n1 0\n-1 2 0\n").unwrap();
        let solver = Solver::new(cnf, 7); // initial assignment is all false

        assert_eq!(solver.clause_stats()[0].state, ClauseState::Rogue);
        assert_eq!(solver.clause_stats()[1].state, ClauseState::Solved);
        // Variable 1 participates in a rogue clause, but its own state is Prime
        // because it is the sole satisfying literal in the second clause.
        assert_eq!(solver.variable_stats()[0].state, VarState::Prime);
        assert_eq!(solver.variable_stats()[0].rogue_clause_occurrences, 1);
    }

    #[test]
    fn neutral_variable_can_still_occur_in_rogue_clauses() {
        let cnf = Cnf::from_dimacs_str("p cnf 2 2\n1 0\n-1 2 0\n").unwrap();
        let mut solver = Solver::new(cnf, 5);
        solver.assignment = vec![false, true];
        solver.rebuild_counts();

        assert_eq!(solver.clause_stats()[0].state, ClauseState::Rogue);
        assert_eq!(solver.variable_stats()[0].state, VarState::Neutral { min_support: 2 });
        assert_eq!(solver.variable_stats()[0].rogue_clause_occurrences, 1);
    }

    #[test]
    fn prime_takes_precedence_over_neutral_support() {
        let cnf = Cnf::from_dimacs_str("p cnf 3 2\n1 2 0\n1 -2 3 0\n").unwrap();
        let mut solver = Solver::new(cnf, 11);
        // Set x1=true, x2=false, x3=false: x1 is sole support in clause 1
        // and also participates in a clause with multiple satisfying literals
        // only if x3 is made true. Keep the direct invariant explicit here.
        solver.assignment = vec![true, false, true];
        solver.rebuild_counts();
        assert_eq!(solver.variable_stats()[0].state, VarState::Prime);
    }

    #[test]
    fn flipping_updates_clause_state_and_variable_incidence_counts() {
        let cnf = Cnf::from_dimacs_str("p cnf 2 2\n1 0\n-1 2 0\n").unwrap();
        let mut solver = Solver::new(cnf, 3);
        assert_eq!(solver.clause_stats()[0].state, ClauseState::Rogue);
        assert_eq!(solver.clause_stats()[1].state, ClauseState::Solved);

        solver.flip(0);
        assert_eq!(solver.clause_stats()[0].state, ClauseState::Solved);
        assert_eq!(solver.clause_stats()[1].state, ClauseState::Rogue);
        assert_eq!(solver.unsatisfied_count(), 1);
        assert_eq!(solver.variable_stats()[0].rogue_clause_occurrences, 1);
    }
}
