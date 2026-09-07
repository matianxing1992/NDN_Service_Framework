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
