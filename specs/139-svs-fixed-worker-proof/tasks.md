# Tasks: Fixed-Rate Single-Worker Proof

- [x] T001 Audit the fixed 600 pps provenance, same-binary/two-mode boundary,
  frozen Spec 137/138 ownership, one-worker runtime path, qualification gates,
  six-cell decision, and task cohesion; record
  `evidence/pre-implementation-audit.md`.

- [x] T002 [US1] Implement and test the thin fixed-rate runner/analyzer in
  `Experiments/NDN_SVS_Fixed_Worker_Proof_Minindn.py`,
  `Experiments/analyze_svs_fixed_worker_proof.py`, and
  `tests/python/test_spec139_svs_fixed_worker_proof.py`; run both fresh 600 pps
  qualifications once and seal nothing unless both pass.

- [x] T003 [US2] Seal and execute exactly six AB/BA/AB 10/60/10 MiniNDN cells
  once under `results/spec139-svs-fixed-worker-proof/<id>/`, preserving every
  receipt without retry.

- [x] T004 [US2] Verify all hashes, emit complete run/pair/stage/traffic/host
  outputs and `evidence/fixed-worker-report.md`, apply the frozen predicate,
  audit post-implementation evidence, verify predecessors unchanged, and
  freeze Spec 139.

```text
T001 -> T002 -> T003 -> T004
```
