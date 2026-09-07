# T005 operator seam checkpoint

Date: 2026-09-07

## Implemented boundary

`Experiments/TigerCluster/runtime/yolo_operator.py::run_rank` is the single
rank-level coordinator between a prepared run and the maintained application
owner. It validates the resolved case/role layout, the canonical
`applicationName + "/sync"` input, digest-shaped preparation and candidate
bindings, sealed directory boundaries, endpoints, timing budgets, and the
CPU/GPU allocation contract. It then constructs `NodeRuntime` through
`NodeRuntime.from_preparation`, creates separately stored startup and completion
barriers with the same run/candidate/probe binding, and delegates to
`apps.yolo.run_normal_node`.

The seam does not synthesize ACKs, Selection, model output, numerical verdicts,
or qualification receipts. The hidden Slurm runner remains fail-closed until
the real allocation, SIF, application, and collector gates are complete.

## Focused evidence

```text
python3 -m pytest -q Experiments/TigerCluster/tests/test_yolo_operator.py \
  Experiments/TigerCluster/tests/test_yolo_bundle.py \
  Experiments/TigerCluster/tests/test_yolo_submit.py --tb=short
40 passed in 9.12s

python3 -m pytest -q Experiments/TigerCluster/tests --tb=short
798 passed in 33.44s
```

The operator tests use a lifecycle double to prove rejection before runtime
construction and exact forwarding of the plan, endpoints, options, collector,
allocation field, and both barrier bindings. Application tests additionally
exercise the normal `configure_network` → `start_workload` → request schedule →
completion → cleanup order and the startup-failure cleanup path with lifecycle
doubles. This is structural evidence only;
no native extension, SIF, model, MiniNDN, GPU, or TigerCluster process ran.

## Remaining work

### 2026-09-07 shared probe and early budget regression

The multi-rank operator now requires a coordinator-provided shared `probe_id`.
Previously each rank could generate a different random ID, making its peer's
otherwise valid startup record fail the binding check. Single-rank invocation
may still generate its own ID. Startup, completion and cleanup budgets are
validated before `NodeRuntime.from_preparation`, including rejection of boolean
and nonfinite values.

`python3 -m pytest -q Experiments/TigerCluster/tests/test_yolo_operator.py
Experiments/TigerCluster/tests/test_yolo_startup.py --tb=short`:
**16 passed in 0.88s**. The positive two-rank test exchanges real on-disk startup
and completion records with one ID. Runtime creation and application lifecycle
are doubles; it does not demonstrate multi-process NFD, model or GPU execution.
The allocation coordinator must still supply this shared ID in the real path.

T005 remains open until the actual secure YOLO application path and collector
are wired to this seam and exercised through T006/T007. T007 must re-audit every
effective profile field and the hidden `run` boundary before any formal build,
SIF promotion, or Tiger submission.
