# US2 Rollback

Verdict: **PASS**

Runtime v1 remains the bounded compatibility interface. Its characterized
score delegates to `planner.cost_policy.score_cost`, and actual assignment
delegates to `CostProviderAssignmentPolicy`; existing frozen goldens pass.
Selecting `DefaultOptimizationSuite` therefore restores pre-external-optimizer
behavior without changing Core contracts, persisted state or wire protocol.
The source-path assertion printed `US2_COST_POLICY_ROLLBACK_OK`.
