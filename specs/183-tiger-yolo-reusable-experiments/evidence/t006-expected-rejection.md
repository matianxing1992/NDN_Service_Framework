# T006 expected-rejection component checkpoint

Date: 2026-09-07. Branch: `TigerClusterExperiments`.

`runtime/yolo_result.py::finalize_expected_rejection` now provides a separate,
fail-closed terminal boundary for the registered `negative-dependency` case.
It does not reuse the normal success verdict and does not infer a rejection
from a timeout or a missing output directory. The retained record must bind the
run, request, attempt and candidate; show one committed Selection with zero
reselection; identify either `DEPENDENCY_DATA_MISSING` or `PEER_FAILURE` on a
specific planned edge after Selection; show no response; and include bounded,
non-forced cleanup with every owned child reaped.

The focused regression includes a valid record and mutations for wrong case,
missing/duplicate Selection, pre-Selection failure, response presence,
reselection, forced cleanup, an unbounded elapsed time, and a wrong candidate.
All mutations reject before a verdict is emitted.

```text
python3 -m pytest -q Experiments/TigerCluster/tests/test_yolo_retained_execution.py \
  --tb=short --junitxml=Experiments/TigerCluster/results/t006-negative-rejection-r1/junit.xml
```

Result: **53 passed in 0.62s**, exit 0. The complete registered focused suite
then passed **852 tests**, exit 0, with JUnit output at
`Experiments/TigerCluster/results/t006-negative-rejection-r1/full-junit.xml`.

This is source-shaped component evidence only. No negative MiniNDN, SIF, Slurm,
or TigerCluster workload was executed, and T006/T007 remain open until the
operator collector consumes a real retained record and the production wiring
audit passes.
