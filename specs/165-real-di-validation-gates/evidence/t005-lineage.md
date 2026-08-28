# T005 Request Lineage Evidence

Status: PASS.

Canonical lineage admission is implemented in
`Experiments/ndnsf_validation/lineage.py`. The application SDK accepts the
User-owned request ID, validates its wire-safe form, retains it through
submission, and rejects a duplicate active ID.

Focused tests cover request/attempt/plan/model/provider-role mismatch,
duplicate, reorder, forgery, and post-terminal events. Closure logs contain
the same canonical IDs in Request, ACK, Selection, and token response paths;
all 128 token requests across host and container evidence satisfy
`requestId == transport.wireRequestId`.
