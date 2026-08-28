# Tasks: Spec 012 DI Closed-Loop Workload Campaign

**Status**: Historical implementation tasks complete; evidence qualification
updated by the 2026-07-21 audit.

## Phase 1: User Story 1 - Sequential Workload Driver (P1)

- [x] T001 [US1] Deliver positive `--requests` sequential closed-loop execution through one ServiceUser, ordered per-request JSON, first-failure stop, aggregate workload/execution output, default-one compatibility, and exact metric calculations in `examples/python/NDNSF-DistributedInference/native_di_tracer/user_driver.py`; acceptance is the deterministic/syntax behavior specified by FR-001..FR-005 and SC-001..SC-004.

## Phase 2: User Story 2 - MiniNDN and Campaign Propagation (P1)

- [x] T002 [US2] Propagate request count and workload fields without unit/identity drift through `Experiments/NDNSF_DI_NativeTracer_Minindn.py` and `examples/python/NDNSF-DistributedInference/native_di_tracer/run_layout_campaign.py`, retaining attributable run rows and layout aggregates; acceptance is FR-006..FR-008 and SC-003/SC-005.

## Phase 3: User Story 3 - Bounded Historical Evidence (P2)

- [x] T003 [US3] Preserve the original full-network smoke command, two-layout/two-run/three-request/75-ms comparison, limitations, and next-step boundary in `specs/012-di-closed-loop-workload/spec.md`, `plan.md`, `traceability.md`, and `audit.md`; acceptance is FR-009..FR-011 and SC-006/SC-007, with numerical results labeled historically reported because the `/tmp` raw artifacts are unavailable.

## Dependencies and Cohesion

T001 precedes T002 because the harness consumes the driver contract. T003
depends on both and owns evidence qualification. Each task combines behavior,
focused validation, and evidence because separating old W001-W008 file/process
steps would recreate mechanical fragmentation.

## Historical Validation Commands

These commands document the original scope. They are not claimed as rerun by
this audit unless explicitly listed in `audit.md`:

```bash
python3 -m py_compile \
  examples/python/NDNSF-DistributedInference/native_di_tracer/user_driver.py \
  Experiments/NDNSF_DI_NativeTracer_Minindn.py \
  examples/python/NDNSF-DistributedInference/native_di_tracer/run_layout_campaign.py

PYTHONPATH=pythonWrapper:NDNSF-DistributedInference python3 \
  examples/python/NDNSF-DistributedInference/native_di_tracer/user_driver.py \
  --dry-run --requests 3

./waf build --targets=di-native-provider
./waf build --targets=di-native-plan-manifest-smoke
```
