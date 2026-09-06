# Spec180 T006 ACK Planning Evidence

**Status**: `PARTIAL_IMPLEMENTATION` (production ACK/Selection qualification remains open)

The generic task client now marks its ACK close policy as `DEADLINE`, so a
caller-provided `ack_coverage_roles` hint cannot close the Spec180 discovery
window early. The legacy V2 path rejects heterogeneous candidate role sets
instead of allowing a `candidates[0]` role contract to control the request; the
V3 path evaluates each candidate with its own role/dependency/resource
requirements and orders feasible candidates by signed adapter priority then
candidate digest. Catalogue list order is not a decision input.

Focused compatibility command:

`PYTHONPATH=NDNSF-DistributedInference python3 -m pytest -q tests/python/test_spec180_generic_request_api.py tests/python/test_spec170_placement_v3.py tests/python/test_spec170_role_dataflow_contract.py`

Result: `17 passed` for the existing compatibility selectors plus
`7 passed` in `tests/python/test_spec180_yolo_ack_planning.py`, covering
catalogue-order permutation, signed-priority ordering, atomic-only and
four-role capability feasibility, distinct Provider ownership, expiry
rejection, canonical DetectShard component-role classification, and
fail-before-enumeration rejection of negative priority. A real production ACK
snapshot with event-order evidence and duplicate-offer handling is still
required before T006 can be marked complete.
