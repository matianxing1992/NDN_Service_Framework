# Spec180 T005 YOLO Adapter Evidence

**Status**: `PARTIAL_IMPLEMENTATION` (candidate qualification remains open)

Implemented `adapters/yolo/{graph,candidates,adapter}.py` and exported the
adapter through the model-family registry. The adapter is ONNX-only at runtime,
requires the canonical graph interface (`images` → `predictions`), validates
the graph revision and external initializer digest, binds catalogue model/graph
digests to the package manifest, verifies the configured catalogue signature
when enabled, and exposes only `atomic-v1` and
`shared-backbone-two-shard-v1` with explicit ingress/egress and Merge roles.
The shared candidate uses only its three signed deterministic graph boundaries;
all-consumer fan-out is included when deriving crossed dependency tensors.
The runtime `SplitCandidate` now carries the signed `input_ingress_role` and
`result_egress_role` fields; both are validated against the execution-plan
roles and therefore participate in `candidate_digest`.

Focused command:

`PYTHONPATH=NDNSF-DistributedInference python3 -m pytest -q tests/python/test_spec180_yolo_adapter.py`

Result: `6 passed` (wrong revision, missing initializer, tampered candidate,
signature verification, digest binding, safe-cut validation, candidate
enumeration, and runtime ingress/egress digest propagation). The remaining closure items are malformed-graph mutation
coverage and ACK-feasibility/application wiring owned by T006/T009; T004's
unsigned authority operation is also still open.
