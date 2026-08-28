# Spec 128 Validation Quickstart

1. Run deterministic retry, two-erasure, capacity-plus-one, stale/stop and
   binding tests; then Core/binding/security suites.
2. Freeze source, binding, workload, runner and baseline hashes; verify one
   MiniNDN owner, qdisc proof, free space and unused `results/spec128-*` root.
3. Execute all 16 manifest cells once through one launcher.
4. Preserve campaign summary, per-cell CSV and failure evidence. Verify Spec
   127 hashes before/after and close only against every Spec 128 criterion.
