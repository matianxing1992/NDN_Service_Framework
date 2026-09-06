# Spec180 T002 Generic Request and Input Transport Evidence

**Status**: PARTIAL for the implementation boundary; the generic envelope,
reference-aware input checks, and coordinator dispatch pass, but the task's
production ingress/egress ownership acceptance is still open.

## Reproduction

```bash
python3 -m pytest -q \
  tests/python/test_spec180_generic_request_api.py \
  tests/python/test_spec180_yolo_security.py \
  tests/python/test_spec175_conversation.py \
  tests/python/test_spec168_provider_generation.py \
  tests/python/test_ndnsf_di_selection_dataflow.py \
  tests/python/test_spec170_placement_v3.py \
  tests/python/test_spec170_role_dataflow_contract.py
```

Result: `19 passed` for the two focused Spec180 generic/security boundary
regressions; the
broader compatibility set remains the prior prerequisite run.

The 20-case `test_spec168_provider_generation.py` compatibility suite also
passes after the V2 ownership-flag repair; this confirms legacy Provider
handlers do not depend on V3-only state.

## Verified boundary

- `ApplicationInput` has mutually exclusive `INLINE` (maximum 4096 bytes) and
  encrypted `REPO_REF` modes.
- Repository references carry only an authenticated NDN name, manifest/content
  digests, size, encryption marker, authorization scope, and non-plaintext
  protection epoch; plaintext is not
  copied into the request.
- `DIRequestEnvelopeV2` serializes the transport mode and reference while
  emitting an empty input payload for `REPO_REF`.
- `InferenceApplication.request()` and `InferenceClient.request_task()` route
  generic model/task requests to the existing coordinator without Provider
  lists, deployment records, or role maps. Existing GenerationInput callers
  remain compatible.
- Provider ACK processing validates the transport boundary only; it does not
  fetch or decrypt repository data before Selection.
- The V3 coordinator projects a YOLO candidate's declared ingress into one
  `TensorEndpoint(source_kind=APPLICATION_INPUT)` and uses its declared egress
  role as the single terminal-response owner; legacy non-YOLO candidates retain
  the graph-derived compatibility path until they declare the pair.
- `ProviderRuntimeContext.fetch_application_input()` decodes bounded INLINE
  input or calls the structured `fetch_large_reference()` path for encrypted
  `REPO_REF`. The reference-aware path requires the absolute name, encrypted
  marker, matching authorization scope, positive plaintext size,
  non-plaintext protection epoch, and a valid
  SHA-256 content digest before returning bytes. Missing reference support,
  metadata failure, or digest mismatch fails closed with
  `DI_INPUT_REPO_DIGEST_UNVERIFIED`; size mismatch remains
  `DI_INPUT_REPO_SIZE_MISMATCH`. `publish_terminal_result()` and stream
  termination enforce the signed terminal-owner flag. Non-owner access fails
  with the stable `DI_INPUT_FETCH_ROLE_MISMATCH` or
  `DI_TERMINAL_RESPONSE_ROLE_MISMATCH` code.
- Focused tests cover owner/non-owner input and terminal publication,
  reference-aware scope/digest/epoch checks, name/size-only rejection, tampered
  content, and size mismatch. Test doubles do not constitute a real
  ACK-to-Provider execution result.
- `tests/python/test_spec180_yolo_security.py` exercises the actual Python
  wrapper `CollaborationContext.fetch_large_reference()` implementation,
  including scope/encryption/digest tampering, and confirms terminal Response
  publication is single-use. These remain focused boundary checks, not a
  signed NDN repository or network execution result.
- The focused set also invokes the legacy `DistributedInferenceProvider.add_role()`
  wrapper after V3 ownership wiring; it no longer depends on undefined V3-only
  state and remains source-compatible.

The selected candidate's ingress-only authorization and reference-aware content
verification are now enforced by the focused Provider boundary, but a
maintained YOLO caller exercising a real
ACK/Selection/Provider/Response path is still owned by T009/T011. This evidence
therefore cannot close T002 by itself. It contains no
model output, input plaintext, network mutation, SIF build, or Tiger result.

## Superseded publication note (audit iteration 34)

The caller-publication status described by this earlier record is superseded by
`evidence/t002-input-publication-implementation-current-20260902.md`. The
source and binding now perform publication and reference binding; the live
ACK/Selection/Provider/Response limitation remains unchanged.
