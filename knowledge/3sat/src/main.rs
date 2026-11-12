use context_3sat::{Cnf, SolveResult, Solver, SolverConfig, verify};
use std::env;
use std::fs;
use std::process::ExitCode;

fn usage() {
    eprintln!("Usage: 3sat INPUT.cnf [--steps N] [--restarts N] [--noise 0..1] [--seed N] [--out FILE]");
}

fn main() -> ExitCode {
    let mut args = env::args().skip(1);
    let Some(input) = args.next() else { usage(); return ExitCode::from(2); };
    let mut config = SolverConfig::default();
    let mut output = String::from("./knowledge/3sat/results/latest.json");
    while let Some(flag) = args.next() {
        let Some(value) = args.next() else { eprintln!("Missing value for {flag}"); usage(); return ExitCode::from(2); };
        let parsed: Result<(), String> = match flag.as_str() {
            "--steps" => value.parse::<usize>().map(|v| config.max_steps = v).map_err(|e| e.to_string()),
            "--restarts" => value.parse::<usize>().map(|v| config.restarts = v).map_err(|e| e.to_string()),
            "--noise" => value.parse::<f64>().map(|v| config.noise = v).map_err(|e| e.to_string()),
            "--seed" => value.parse::<u64>().map(|v| config.seed = v).map_err(|e| e.to_string()),
            "--out" => { output = value; continue; },
            _ => { eprintln!("Unknown option: {flag}"); usage(); return ExitCode::from(2); }
        };
        if let Err(e) = parsed { eprintln!("Invalid value for {flag}: {e}"); return ExitCode::from(2); }
    }
    if !(0.0..=1.0).contains(&config.noise) { eprintln!("--noise must be between 0 and 1"); return ExitCode::from(2); }

    let cnf = match Cnf::from_dimacs_file(&input) {
        Ok(cnf) => cnf,
        Err(e) => { eprintln!("Failed to load CNF: {e}"); return ExitCode::from(1); }
    };
    if !cnf.is_3sat() { eprintln!("Warning: input contains clauses longer than 3 literals; solving general CNF instead of strict 3-SAT."); }
    let mut solver = Solver::new(cnf.clone(), config.seed);
    let result = solver.solve(&config);
    let json = match &result {
        SolveResult::Sat { assignment, steps, restart } => {
            if !verify(&cnf, assignment) { eprintln!("Internal verification failed"); return ExitCode::from(1); }
            let values = assignment.iter().map(|v| if *v { "true" } else { "false" }).collect::<Vec<_>>().join(", ");
            format!("{{\n  \"status\": \"SAT\",\n  \"variables\": {},\n  \"clauses\": {},\n  \"steps\": {},\n  \"restart\": {},\n  \"assignment\": [{}]\n}}\n", cnf.n_vars, cnf.clauses.len(), steps, restart, values)
        }
        SolveResult::Unknown { steps, best_unsatisfied } =>
            format!("{{\n  \"status\": \"UNKNOWN\",\n  \"variables\": {},\n  \"clauses\": {},\n  \"steps\": {},\n  \"best_unsatisfied\": {}\n}}\n", cnf.n_vars, cnf.clauses.len(), steps, best_unsatisfied),
    };
    if let Some(parent) = std::path::Path::new(&output).parent() {
        if !parent.as_os_str().is_empty() {
            if let Err(e) = fs::create_dir_all(parent) { eprintln!("Cannot create output directory: {e}"); return ExitCode::from(1); }
        }
    }
    if let Err(e) = fs::write(&output, json) { eprintln!("Cannot write result: {e}"); return ExitCode::from(1); }
    match result {
        SolveResult::Sat { steps, restart, .. } => { println!("SAT ({} steps, restart {}) — {}", steps, restart, output); ExitCode::SUCCESS }
        SolveResult::Unknown { steps, best_unsatisfied } => { println!("UNKNOWN ({} steps, best {} unsatisfied) — {}", steps, best_unsatisfied, output); ExitCode::from(3) }
    }
}
