# Spec175 Local Qualification Plan

## Question

Does the current implementation satisfy the generic streamed-invocation and
stateful Qwen contracts through the production network path before packaging or
hardware effects are introduced?

## Cases

| Case | Purpose | Required result |
|---|---|---|
| Cold generation | Prove one Request/ACK/Selection, prefill, automatic decode, ordered Tokens, and terminal Response | Exact deterministic token/result oracle and clean termination |
| Conversation continuation | Prove the second turn binds and reuses the first turn's role-local state | Two complete turns, authenticated checkpoint continuity, no model-state transfer |
| Checkpoint mismatch | Prove continuation fails closed | Explicit rejection before mixed or partial state is executed |

These are functional qualification cases, not samples for statistical analysis.
Latency and throughput may be recorded only as diagnostics.

## Evidence rule

All cases must use the same post-audit source and effective configuration.
Record exact commands, source/dependency hashes, request/attempt/plan lineage,
token/result oracles, child exits, and cleanup. Do not substitute an isolated
fixture, historical SIF, CUDA readiness probe, or Tiger job.
