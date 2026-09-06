# T011 / S2b — Y-B Live Four-Role Terminal Response (Revision 115) — 2026-09-04

> **INVALIDATED — DO NOT REUSE.** Revision 123 changed the Controller
> PUBPARAMS readiness/runtime source after this terminal run (spec181 R003
> banner audit).  The current-source Y-B wiring record is
> `t011-y-b-live-current-20260905-r22.md`.  This historical first-terminal
> PASS is retained for audit history only and cannot satisfy a new Y-B gate.

**Status**: `SPEC180_CASE_RESULT status=PASS case=Y-B` (runner exit 0). The
shared-backbone-two-shard-v1 candidate executed end-to-end across four
Provider identities with a distinct capability cover.

## Sealed inputs

- Candidate package: `.codex-tmp/spec180-yolo-candidate-current` (r112 seal)
- Case bundle: `.codex-tmp/spec180-yolo-y-b-inputs-r112/` (four offer keys,
  Providers spread across ucla/neu/arizona/wustl to avoid the shared
  node-scoped PIB race)
- Evidence root: `.codex-tmp/spec180-yolo-y-b-live-output-r112/`

## Observed sequence (lifecycle.jsonl)

1. `INPUT_REFERENCE_PUBLISHED`
2. `REQUEST_SENT` placement=V3 mode=DEFERRED
3. `ACK_CLOSED` ackCount=4 — one candidate-bound V3 offer per role
4. `GRAPH_READY`
5. `PLACEMENT_DECISION` candidateId=shared-backbone-two-shard-v1
   candidateDigest=`sha256:57547ffcc35430d65551bb0762d3b33f99567583f9c06e587ecd5baf5823dbb9`
   providerCount=4
6. `ARTIFACTS_READY` artifactCount=4
7. `PLAN_SEALED`
8. `SELECTION_COMMITTED` selectedRoleCount=4
9. `PROVIDER_EXECUTION_STARTED` providerCount=4
10. `TERMINAL_RESPONSE` status=True resultDigest=
    `sha256:e6f942bc3e9d35409f8694b732732c542b108bad2a9ede4737eb2665f8dc23aa`

## Four-role execution (provider logs)

| role | nodes | observed |
| --- | --- | --- |
| BackboneNeck | 405 | `YOLO_LAYOUT_FIRST outputs=tensor-0,tensor-1,tensor-2` |
| DetectShard0 | 20 | `YOLO_LAYOUT_INTERMEDIATE outputs=tensor-3,tensor-6` |
| DetectShard1 | 40 | `YOLO_LAYOUT_INTERMEDIATE outputs=tensor-4,tensor-5,tensor-7,tensor-8` |
| Merge | 126 | `YOLO_LAYOUT_FINAL output=(1, 300, 6)` |

Every role assembled its certified subgraph from the canonical root
(`assemble_certified_onnx_model` + digest-pinned `CertifiedOnnxAssemblyRecipe`
from the sealed V3 projection), verified the canonical ONNX graph/initializer
identity, loaded the assembled bytes through CPU ONNX Runtime, and exchanged
activation tensors through the encrypted NDNSF_DATA_V1 dependency dataflow.

## Numerical equivalence (executed, not claimed)

The four-role terminal result digest is byte-identical to the independent
offline full-model CPU-ORT forward on the same 640-by-640 deterministic
input, and to the Y-A atomic result:

- Y-B live:   `sha256:e6f942bc3e9d35409f8694b732732c542b108bad2a9ede4737eb2665f8dc23aa`
- Y-A live:   `sha256:e6f942bc3e9d35409f8694b732732c542b108bad2a9ede4737eb2665f8dc23aa`
- offline full model: identical digest

## Code changes this revision (each with a focused test)

- Y-B/Y-N Providers bind the same validated local canonical package
  (`--deployed-models --local-model-path`); the repository-materialization
  transport remains the SIF/Tiger promotion path.
  `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`.
- `YoloCanonicalArtifactBinding` (describe/ensure port): digest facts for
  recipe certification and the published ARTIFACT identities.
  `adapters/yolo/adapter.py`.
- `CanonicalArtifactBinding.canonical_graph_digest`: the assembler verifies
  the canonical ONNX identity digest, which differs from the planning-space
  graph-port digest.
  `app_sdk/canonical_artifacts.py`, `app_sdk/placement.py`.
- The Provider assembles certified component-set subgraphs after Selection
  and swaps the assembled model into the role execution.
  `ndnsf_distributed_inference/provider.py` +
  `tests/python/test_spec180_role_assembly.py`
  (real-package behavioral test: assembled interface names match the
  certified contracts).
- The maintained User wires `canonical_artifact_ensurer` into the
  coordinator. `examples/.../yolo_2x2/user.py`.

## Boundary

Local MiniNDN Y-B evidence. The Y-N critical controls, T014 audit, T015
inventory, SIF replay, and Tiger submission are separate steps.
