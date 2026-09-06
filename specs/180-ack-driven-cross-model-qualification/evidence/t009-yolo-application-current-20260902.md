# Spec180 T009 YOLO application entrypoint evidence

**Status**: PARTIAL implementation checkpoint; not a production or
qualification result.

## Verified

- `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py` now has a
  model-first `_load_yolo_ack_driven()` entrypoint.
- The model-first entrypoint constructs `ModelRef`, `InferenceTaskRef`, and
  encrypted `ApplicationInput.REPO_REF`, configures `PreSplitFirstStrategy`,
  closes the registered ACK window (`ack_coverage_roles=()`), and calls the
  existing `APPClient.request_task()` coordinator. The live capability
  authority for completion is the network `CollaborationAckClosed` snapshot;
  the signed APP Data catalogue record is only artifact-publication metadata
  and is not a discovery or candidate-selection authority. The former
  file-backed snapshot is now restricted to the explicit offline oracle.
- The maintained qualification profile now defaults to and enforces the
  registered `ack-timeout-ms=1500` value; non-profile values fail before a
  request is sent.
- The entrypoint requires explicit canonical-package, catalogue-registry,
  ACK-offer-key, catalogue APP Data name, and expected
  catalogue signer before it starts a request; it does not accept a Provider
  list, split ID, role map, or `auto_parallel_detect_plan` result as authority.
- The maintained caller publishes the encoded input through the canonical APP
  publication method (from `--input-payload-file` or the deterministic fixture)
  and rejects a bare `--input-reference-file` in ACK-driven mode.
- The active catalogue is now fetched through
  `NetworkCatalogSnapshotResolver`, which uses exact-name signed APP Data and
  verifies the canonical snapshot digest. It is invoked only after the
  coordinator closes the network `CollaborationAckClosed` snapshot.
- The historical service-policy `distributed_inference()` and asynchronous
  calls remain available only after the explicit `--offline-oracle` flag.
- The maintained Provider handler now calls
  `ProviderRuntimeContext.fetch_application_input()` for the V3 ingress
  boundary and `publish_terminal_result()` for the candidate-declared egress;
  the old encrypted-reference helper is limited to the explicit offline
  compatibility fallback.
- `tests/python/test_spec180_yolo_application.py` passes 4 source-contract
  tests, including the Provider ownership API wiring check.

## Still open

The iteration-35 source-bound publication refresh is additionally covered by
`evidence/t002-input-publication-implementation-current-20260902.md` and the
historical 71-test Spec180 collection; the current collection is larger after
later inventory/local-gate additions. It does not change this file's partial
status.

The canonical package still needs a registered production signature and active
artifact publication/assembly. Provider-offer verification still uses the
caller key-map scaffold and must be replaced or bound to production trust
configuration before qualification. The real ACK-to-Selection-to-native-Merge
request, reference-aware digest-verifying ingress fetch, terminal result oracle,
and security negatives remain T005--T010 work. This file contains no model output, input plaintext,
SIF build, or Tiger result and must not be used as qualification evidence.
