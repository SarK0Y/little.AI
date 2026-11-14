/* Sequential rogue groups + neutral-driven merge (layered on top of sat.h).
 *
 * Formation. Start from the majority-polarity assignment B.
 *   1. G1 = ALL clauses that are rogue under B (or, with group_len = L > 0, the L
 *      rogue clauses with the lowest ids; the rest wait for later groups).
 *      Solve G1 on its own (a
 *      sub-formula that sees only G1's clauses, started from B), which gives
 *      a snapshot X1 in which the same variables may carry other polarities.
 *   2. Apply X1 to B, inspect the WHOLE formula, and collect ALL clauses that
 *      are rogue now into G2. Solve it independently, keep its snapshot X2.
 *   3. Repeat until no rogue clause is left (then B already solves the
 *      formula), or the group cap / a repeated rogue set / an unsolvable
 *      group stops formation. (Two groups that share variables can undo each
 *      other forever; `diversify` re-solves such a repeat with random picks.) Snapshots of earlier groups are never
 *      overwritten.
 *
 * Merge (only if formation did not close the formula).
 *   - Start from the snapshot with the most neutral variables.
 *   - At every step, the *source* group is the one with the most fireable
 *     neutrals: a variable is fireable if it has zero breaks under the merged
 *     assignment, has not fired yet, sits in a still-rogue clause of that group
 *     and the group's snapshot wants the opposite polarity (directed flip).
 *     Each variable fires at most once for the whole merge; firing re-ranks
 *     everything, so a prime can turn neutral (and a neutral can turn prime)
 *     between steps.
 *   - When no directed flip is left, any other unfired neutral inside a rogue
 *     clause may still fire (free flip): it cannot break anything and fixes
 *     the clause. `directed_only` disables this tier.
 *   - A zero-break flip never un-solves a clause, so the rogue count strictly
 *     falls with every flip and the merge always terminates.
 *   - If neutrals run out, we stop and report: unmerged clauses, neutral and
 *     prime variables, and the merged assignment. We never claim SAT unless the
 *     merged assignment verifies against the whole formula.
 */
#ifndef C3SAT_GROUPS_H
#define C3SAT_GROUPS_H

#include "sat.h"

#define GROUPS_MAX 1024  /* each group keeps a full snapshot: memory ~ groups * n_vars bytes */

typedef struct {
    size_t max_groups;  /* formation cap, clamped to 1..GROUPS_MAX */
    size_t group_len;   /* max clauses per group; 0 = unlimited (ALL currently rogue clauses) */
    bool directed_only; /* merge: only flips that a group's snapshot asks for */
    size_t diversify;   /* formation: how often a repeated rogue set may be re-solved with full
                           noise before formation stops (0 = stop at the first repeat) */
} GroupConfig;

GroupConfig group_config_default(void);

typedef struct {
    SolveStatus status;        /* SOLVE_SAT, SOLVE_UNKNOWN, or SOLVE_UNSAT (unit-pair pre-check) */
    bool *assignment;          /* owned; merged assignment (for UNKNOWN: the merged part) */
    size_t n_vars;
    size_t steps;              /* local-search flips spent solving groups */
    size_t n_groups;
    size_t anchor;             /* group the merge started from (valid if merged) */
    bool merged;               /* true if the merge phase ran */
    size_t merge_flips;        /* one-shot neutral flips fired during the merge (all tiers) */
    size_t merge_flips_free;   /* ...of which free flips (not asked for by any group) */
    size_t *group_sizes;       /* owned; clauses per group */
    size_t *group_off;         /* owned; CSR over groups (n_groups + 1 entries) ... */
    size_t *group_clauses;     /* owned; ... clause ids of group k: group_clauses[group_off[k]..group_off[k+1]) */
    size_t *rogue;             /* owned; clauses still unsatisfied (0-based ids) */
    size_t n_rogue;
    size_t *rogue_group_off;   /* owned; CSR over `rogue` (n_rogue + 1 entries) ... */
    size_t *rogue_group;       /* owned; ... ascending 0-based ids of the groups unmerged clause i was rogue in */
    size_t *neutral;           /* owned; variables with zero breaks under `assignment` */
    size_t n_neutral;
    size_t *prime;             /* owned; variables that are sole support of a solved clause */
    size_t n_prime;
    Contradiction *contradictions; /* owned; UNSAT only */
    size_t n_contradictions;
} GroupResult;

typedef RESULT(GroupResult) ResultGroup;

ResultGroup group_solve(const Cnf *cnf, const SolverConfig *cfg, const GroupConfig *gcfg);
void group_result_free(GroupResult *r);

#endif
