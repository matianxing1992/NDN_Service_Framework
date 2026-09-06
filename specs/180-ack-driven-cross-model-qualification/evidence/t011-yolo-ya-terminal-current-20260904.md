# T011 / S2a — Y-A Live Terminal Response (Revision 112) — 2026-09-04

> **INVALIDATED — DO NOT REUSE.** Revision 123 changed the Controller
> PUBPARAMS readiness/runtime source after this terminal run (spec181 R003
> banner audit).  The current-source Y-A wiring record is
> `t011-y-a-live-current-20260904.md`.  This historical first-terminal PASS
> is retained for audit history only and cannot satisfy a new Y-A gate.

**Status**: the first real Y-A ACK-driven terminal Response in this feature's
history. `SPEC180_CASE_RESULT status=PASS case=Y-A` (runner exit 0).

## Sealed inputs

- Candidate package: `.codex-tmp/spec180-yolo-candidate-current` (revision-112
  re-sign with the registered catalogue authority; see
  `s1-candidate-seal-current-20260904.md`)
- Case bundle: `.codex-tmp/spec180-yolo-y-a-inputs-r112/` (fresh r112 seal)
- Evidence root: `.codex-tmp/spec180-yolo-y-a-live-output-r112/`
- Provider: exactly one identity `/example/provider/FullModel` with a
  validated local canonical ONNX package (`--local-model-path`), a
  candidate-bound V3 offer key, and the registered 1500 ms ACK window.

## Observed sequence (lifecycle.jsonl, schema
`spec180-yolo-lifecycle-event-v1`)

1. `INPUT_REFERENCE_PUBLISHED` — encrypted REPO_REF published by the
   maintained caller through `APPClient.publish_application_input_reference()`
   before `REQUEST_SENT`
2. `REQUEST_SENT` — generic model-first request, placement=V3, mode=DEFERRED
3. `ACK_CLOSED` ackCount=1 — one candidate-bound V3 offer accepted
   (`NDNSF_DI_ACK_DECISION status=true reason=DI_PLACEMENT_V3_OFFER`)
4. `GRAPH_READY` — adapter graph identity bound
5. `PLACEMENT_DECISION` candidateId=atomic-v1 candidateDigest=
   `sha256:7420467646e9a9350b0182e2c85f12dea85833eb0c93c3b119f766462b2814b5`
6. `ARTIFACTS_READY` — PRE_SPLIT resolution against the signed network
   catalogue snapshot (no generated-split materialization)
7. `PLAN_SEALED` planDigest=`sha256:ab13315863d89ad52e876abc3bdbcbfcc482ee839f2edb30a6167...`
8. `SELECTION_COMMITTED` — one authenticated Selection delivered to the
   Provider (`NDNSF_COLLAB_ASSIGNMENT_SELECTED role=FullModel`)
9. `PROVIDER_EXECUTION_STARTED` providerCount=1
10. `TERMINAL_RESPONSE` status=True resultDigest=
    `sha256:e6f942bc3e9d35409f8694b732732c542b108bad2a9ede4737eb2665f8dc23aa`

The lifecycle journal passed `validate_complete()` before the result line was
printed.

## Numerical equivalence (executed, not claimed)

The provider executed the canonical `yolo26n.onnx` (external initializers)
on CPU ONNX Runtime with the deterministic 640-by-640 input
(`make_input(640)`, seed 20260528) and returned
`YOLO_LAYOUT_FINAL role=FullModel output=(1, 300, 6)` (7718 payload bytes).
An independent offline full-model CPU-ORT forward on the same input produced
a byte-identical canonical payload:

- live response resultDigest: `sha256:e6f942bc3e9d35409f8694b732732c542b108bad2a9ede4737eb2665f8dc23aa`
- offline full-model payload sha256: `sha256:e6f942bc3e9d35409f8694b732732c542b108bad2a9ede4737eb2665f8dc23aa`

## Code changes this revision closed (each with a focused red→green test)

- Controller publication ServiceUser stays alive for the whole case and
  publishes with a 10-minute freshness horizon (was: stopped right after
  self-readback, so the signed catalogue vanished from the network).
  `examples/python/NDNSF-DistributedInference/yolo_2x2/controller.py`;
  `tests/python/test_spec180_yolo_application.py`.
- Runner publishes snapshots with the adapter's runtime candidate digest and
  runtime graph identity (was: raw catalogue row digest and ONNX file
  digest, forcing the GENERATED split path).
  `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`.
- `ExecutionRole` accepts the contract-required component-set layer interval
  `layer_begin=0, layer_end=0` while range/rank roles still require a
  strictly increasing interval.
  `NDNSF-DistributedInference/ndnsf_distributed_inference/sdk/placement.py`;
  `tests/python/test_spec180_role_assembly.py`.
- A V3/V2 offer issuer disables the single-role simple-service mirror so the
  Provider runs the V3 collaboration path with dataflow ownership.
  `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/facades.py`;
  `tests/python/test_spec180_yolo_application.py`.
- Provider-prepared roles (accepted `ACCEPT_WITH_PREPARATION` / exact
  residency) carry an empty external fetch reference while the canonical
  artifact identity stays bound in the sealed plan.
  `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py`
  and `.../plan.py`; `tests/python/test_spec180_yolo_application.py`.
- Native `sha256DigestString` in `ServiceProvider.cpp` is canonical lowercase
  (was uppercase, breaking every externalized-assignment digest check).
  Rebuilt with `./waf -o build-system-j2 build -j2` (124/124, ~80 s).
- `/SERVICE`-scoped large-data fetches route through the service-authorized
  NAC-wrapped-key path instead of the collaboration scope-key path.
  `ndn-service-framework/ServiceProvider.cpp` (same -j2 rebuild).
- The Y-A user command pins `--input-size 640` (the canonical graph is
  640-by-640 static; the legacy 32-by-32 default cannot feed it).

## Residual flake (known, not a Y-A blocker)

The MiniNDN-generated `nfd.conf` grants `faces/fib/cs/strategy-choice`
privileges but not `rib`, so prefix-registration commands are authorized by
the default offline validator; on some runs the second ServiceUser's
registrations are rejected before its signer certificate reaches the local
store. It recurs with ~30–50% probability and is a qualification-environment
config gap, not a protocol defect. Tracked for the T014 audit; a rerun of the
same sealed inputs passes.

## Boundary

This is one live MiniNDN Y-A terminal Response. Y-B, Y-N controls, the
T014 audit, SIF replay, and Tiger submission are separate evidence steps.
