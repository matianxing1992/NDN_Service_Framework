# Certified graph producer gap — 2026-09-07

Status: OPEN, implementation finding; not a runtime failure or qualification.
Source inspected: `bef847ef` on `TigerClusterExperiments`.

## Observed boundary

`runtime/yolo_result.py::collect_normal_verdict` requires a certified graph.
`validate_certified_graph_coverage` compares expected role model/artifact
digests, backend and optimized node names with retained native observations.
It does not authenticate the expected graph or establish its origin.
`runtime/yolo_operator.py::finalize_normal_collection` and
`jobs/yolo/submit.py::_collect` accept and forward this object; they do not
produce it. Exact schema/field searches in Tiger and DI source found the
comparator and synthetic test objects, but no production serializer for
`tiger-yolo-certified-graph-v1`.

The previous comparator docstring incorrectly described its producer as
already present. A valid signed catalogue is not, by itself, the missing
optimized-node reference. Existing fixture passes prove comparison behavior,
not that a real invocation can supply independently justified expectations.

## Required completion within T005/T006

1. Trace the existing DI role assembly and ORT session owner. Establish how
   its exact assembled model and optimizer settings determine node identity;
   explicitly map graph node names to profiling event names. Do not assume
   raw ONNX names equal ORT kernel-event names.
2. Implement preparation of expectations independently of the measured
   execution observations, bound to source/runtime, canonical package,
   placement, role model bytes, backend and optimizer configuration.
   Runtime-generated publication/manifest identities must be bound after
   publication; do not demand those per-run identities in a pre-run static
   profile or confuse them with catalogue/placement digests.
3. Connect this producer through preparation, run and collection. Recheck
   retained provenance before invoking the comparator. Never copy measured
   assignments into expected assignments, accept caller assertions as trust,
   remove the coverage requirement or mark missing coverage as PASS.
4. Test the connected path and mutations of role, runtime, model bytes,
   optimizer/backend, node coverage and reference provenance. Then obtain
   actual model evidence at the existing unit/integration/MiniNDN/SIF gates.

This is part of the existing task scope, not an additional campaign or a
reason to wait for a replacement signed legacy model-summary document.
G2/T007 remains unqualified until this and the other production seams close.

## Corrected model/postprocess boundary

Further source tracing found a separate impossible acceptance condition:
`NativeYoloMergeRunner` requires a pathless `native-yolo-postprocess` runner,
and `validate_native_observation` correctly rejects ORT assignments for it.
Nevertheless, the graph join and final verdict required four ORT roles.
Source-shaped final fixtures reproduced three failures (CPU, single GPU,
two GPU), all `FINAL_VERDICT_GRAPH_BINDING`.

The join now validates the three ONNX roles against independent ORT coverage
while retaining all four native execution and dependency checks. The final
verdict likewise requires three ORT roles plus all four execution roles and
the unchanged numerical oracle. Merge is not exempted from execution or
model/dependency identity checks. Missing Merge, failed Merge, missing shard,
and an invented Merge ORT graph are negative cases.

Regression: 153 focused tests passed in 12.72s (retained execution, native
observation, collection and CLI). The new join tests use synthetic retained
readers with the real comparator; no real inference was run. The independent
producer gap above is still open. Its optimized-node reference concerns only
the three ONNX roles, not the native Merge postprocessor.

## Independent ORT preparation component

`runtime/yolo_graph_reference.py::prepare_role_reference` now derives optimized
node expectations from digest-checked inline assembled model bytes without
executing inference or reading observations. It requires an exact ORT version
and explicit CPU/CUDA backend; BASIC optimization, one intra-op thread and
CUDA fallback disabling mirror the native runner (source-parity regression).
Private temporary optimized models are removed, including failure paths;
external-data tensors and control-flow subgraphs are rejected rather than
implicitly traversed. Only digests and node names are returned.

A real Add+Identity CPU test under onnx 1.17.0 / ORT 1.19.2 independently
executed the model after reference preparation and matched the profiler
vocabulary, including removal of Identity. Ten new tests plus existing
observation/retained tests: 118 passed in 1.38s. This is a tiny-model component
test, not the workload, C++/Python ABI parity, SIF or GPU qualification.

The output is deliberately ORT_GRAPH_PREPARATION_COMPONENT_ONLY. Actual
certified-assembly and publication consumers are still not wired: the caller
must establish input/manifest provenance, target runtime/backend/hardware
equivalence and retained reference binding. The missing producer seam is
partially implemented, not closed. CUDA behavior remains unverified here.

Reference: [ORT graph optimizations](https://onnxruntime.ai/docs/performance/model-optimizations/graph-optimizations.html)
documents initialization-time export and the need for matching target options
and hardware. Do not reuse a host CPU reference as a target GPU reference.

## Connected certified-graph document owner

`runtime/yolo_graph_reference.py` now owns the certified-graph document as well
as the per-role component:

- `serialize_certified_graph(role_references, *, graph_digest)` is the
  production serializer for `tiger-yolo-certified-graph-v1`, the only shape the
  retained comparator accepts. It consumes role-reference records exactly as
  `prepare_role_reference` returns them and never reads execution observations.
  All three ONNX roles must be present (a document silently dropping one role
  would let a missing head evade coverage), and one backend is required across
  roles because the optimized node vocabulary is backend-specific. Each
  reference record must carry its producer schema/qualification, exact ORT
  version, optimizer-export digest and the canonical session options.
- The returned document embeds `referenceProvenance` (per-role producer
  schema/qualification, ORT version, `optimizedModelDigest`, session options,
  backend) alongside the comparator-shaped role expectations.
- `validate_certified_graph_provenance` rechecks that provenance for every
  covered role. `validate_certified_graph_coverage` now invokes it before any
  coverage comparison, so an expected graph fabricated at collection time or
  stripped of its producer identity is rejected instead of trusted
  (CERTIFIED_GRAPH_PROVENANCE_*). Re-deriving the optimized node vocabulary
  from the export digest still requires re-running preparation offline; that
  remains an audit/mutation activity, not an inference from the document.
- Role-boundary parity is regression-guarded: `_ORT_ROLES` equals
  `yolo_result._ORT_ROLES` and `yolo_worker.MODEL_ROLES`.

New tests: real tiny Add-only ONNX role models for all three roles, real CPU
references, serializer document shape and provenance embedding, comparator
join with synthetic retained observations matched only after the expected
names were fixed by ORT preparation, provenance-required comparator gate,
role-coverage gaps/extras, tampered reference identity/provenance, mixed or
unknown backends, node-vocabulary mutations, graph-digest rejection and
deterministic provenance digest. 33 graph-reference tests passed; full
TigerCluster suite 895 passed in 43.27s. Old comparator fixtures are now
explicitly labeled synthetic provenance.

Still open on this gap (unchanged for T005/T006/T007): the real invocation
point that reads role model bytes from a signed candidate package and calls
`prepare_role_reference` on the execution target (CPU locally, CUDA on the
allocated device, never the login node), binds its records with the actual
graph digest after catalogue publication, and carries the serialized document
through preparation/run/collection. That wiring awaits the T001 signed
candidate model manifest and the SIF/GPU gates; no runtime or qualification
claim is made here.
