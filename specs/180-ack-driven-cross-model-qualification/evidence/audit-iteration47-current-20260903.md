# Spec180 audit evidence — iteration 47

Date: 2026-09-03

## Scope

This checkpoint re-audited the Spec180 documents against the current
ServiceUser/pybind ACK path. It repaired the written trust-boundary contract
only; it did not change runtime source and did not start MiniNDN, SIF, or Tiger.

## Finding

The configured NDNSF Trust Schema validates the incoming ACK Data before the
decrypted `RequestAckMessage` is admitted. The current `AckCandidate` projection
then retains the ACK name and message but drops the validated Data signer/key-
locator provenance and wire digest. A production `ProviderOfferV3` verifier
therefore cannot yet bind the inner offer signer to the authenticated Provider.
This is recorded as A180-74 (HIGH, open) and owned by T006.

## Repairs

- `spec.md` FR-004/iteration-47 correction requires provenance to flow from the
  validated Data packet and rejects reconstructed or caller-supplied identity.
- `plan.md` ownership and iteration-47 boundary identify ServiceUser/pybind as
  the projection owner.
- `tasks.md` T006 names the exact C++/pybind/Python paths and negative cases.
- `contracts/ack-driven-yolo-v1.md`, `contracts/yolo-minindn-runner-v1.md`, and
  `data-model.md` define the non-secret provenance fields and fail-closed rule.
- `traceability.md` and the checklist now point to this evidence file.

## Gate status

The verdict remains **CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL VALIDATION
BLOCKED**. T001, T003, and T012 are complete; T002 and T004--T010 remain
partial; T013 is partial; T011 and T014--T020 remain open. The focused HMAC
verifier is fixture-only until T006 wires the production provenance path.

