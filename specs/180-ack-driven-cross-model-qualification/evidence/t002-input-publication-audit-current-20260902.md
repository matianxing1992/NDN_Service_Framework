# T002/T009 Input-Publication Audit

**Date**: 2026-09-02
**Status**: BLOCKING IMPLEMENTATION GAP
**Scope**: maintained YOLO `REPO_REF` caller only

## Verified source facts

- `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py` requires
  `--input-reference-file` in `_load_yolo_ack_driven()`.
- That helper reads the JSON and constructs `ApplicationInput.from_repo_ref()`.
- It does not call `APPClient.publish_large_payload_reference()` or
  `publish_large_payload()` before `client.request_task()`.
- The existing facade already provides
  `APPClient.publish_large_payload_reference()` and the legacy offline path
  uses it, so no second repository protocol is required.
- The facade's legacy wire reference contains the Data name, object ID, size,
  encryption bit, and digest, while DI's `LargeDataReference` additionally
  requires `manifestDigest`, `authorizationScope`, and `protectionEpoch`.

## Consequence

The current helper proves only that a caller can parse reference metadata. It
does not prove that the encrypted object was published for the request, and it
cannot produce the required `INPUT_REFERENCE_PUBLISHED < REQUEST_SENT` event
ordering. Existing focused tests therefore remain implementation evidence, not
an end-to-end input-publication or ACK-to-Response result.

## Required correction

T002/T009/T011 must publish the encoded image through the existing facade (or
consume a source-bound receipt produced by that exact call), obtain and verify
the missing manifest digest, authorization scope, and protection epoch through
the trusted DI binding, bind the complete reference to the request, and record
only non-secret reference fields. The selected candidate-declared ingress role
remains the only post-Selection role allowed to fetch/decrypt/verify plaintext.
A bare `--input-reference-file` or legacy wire reference must remain
offline-fixture input and cannot satisfy the local, SIF, or Tiger qualification
gates.

No qualification test was started from this audit because the production
publication boundary is not yet implemented.
