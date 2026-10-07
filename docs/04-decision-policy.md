# Decision policy

This is the rule the offline numbers were measured with. Implement it exactly first; improvements come after the
live comparison (M4).

## Per transaction

1. **Gate on encoding.** The transaction framed and encoded without error, and its ALT tables resolved.
2. **Touched pools.** `P` = pools of the labelled venues among the resolved keys.
3. **Candidates.** The union over `p ∈ P` of `pool_routes[p]` (each pool keeps its top 192 routes by train support).
   Merge by route id keeping the highest support; sort by support descending, then route id ascending. If there are
   none, stop: the transaction is never sent.
4. **Score.** `p_arb` = arb classifier output; `size` = size model output (log lamports);
   `EV = p_arb × exp(size)` (expected net profit in lamports if we send).
5. **Decide.** Send if `EV ≥ threshold[budget]` from `thresholds.json`. The budget is a send-rate target in
   decisions per chain hour; the threshold was fitted on the validation window to produce that rate.
6. **Packs.** Take the top `3 × L` candidate routes (L from `thresholds.json` for that budget) and group them into
   `L` packs of 3 consecutive routes. One decision sends `L` transactions.

## Budgets in the bundle

`thresholds.json` holds one entry per budget (30, 100, 250, 1,000, 4,000 and 16,000 decisions per hour), each with
`ev_threshold` and `L`. Start M3 at **1,000 decisions per hour**, the best net operating point offline.

## What "captured" meant offline

A decision captures a positive transaction when at least one of its `3 × L` routes is in that transaction's set of
profitable routes from the labeller. Value is the transaction's best labelled net profit (lamports, WSOL routes,
lower bound). Net subtracts 5,000 lamports for each of the `L` transactions sent. Pack feasibility (accounts, CU,
message size) was **not** checked for these packs; M5 measures it.

## Known simplifications to revisit after M4

- Routes inside a pack are not checked for account or compute-unit limits.
- No tip or priority-fee model; profits are labeller lower bounds.
- Only three venues and WSOL bases are labelled. The candidate set will grow when the rebuilt labeller lands.
- The budget is a rate target only. A live controller (adjust the threshold to hold the rate) is a good addition,
  but log the threshold used for every decision.
