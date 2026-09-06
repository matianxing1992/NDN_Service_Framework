# Spec180 Audit Evidence — Iteration 48

Date: 2026-09-03

## Scope

This checkpoint records the narrow T006 implementation repair after iteration
47 identified loss of Trust-Schema-validated ACK provenance at the C++/pybind
boundary. It is not a convergence, MiniNDN, SIF, CUDA, Tiger, or performance
result.

## Implemented projection

- `ServiceUser` derives signer identity and typed KeyLocator reference from the
  validated ACK Data packet and records a SHA-256 digest of its complete wire.
- `AckSelectionCandidate`, pybind `AckCandidate`, and Python `AckCandidate`
  preserve the three non-secret fields.
- Direct/legacy fixtures remain source-compatible and expose empty provenance;
  Python does not reconstruct identity from the ACK name, payload, or key map.

## Focused evidence

- `tests/python/test_spec180_ack_provenance.py` covers field preservation and
  legacy native-fixture defaults plus fail-closed V3 offer identity/digest
  checks (11 focused tests with the planning regression).
- `./waf build --targets=unit-tests -j2` completed successfully (104/104,
  6m37.716s; `build/unit-tests` SHA-256
  `cba281e943d2134ea39fb4dfc8d73d7edd9407b8096d24ac1456154cce36feb3`).
- The rebuilt Python 3.8 extension imports the new fields from
  `pythonWrapper/ndnsf/_ndnsf.cpython-38-x86_64-linux-gnu.so` (SHA-256
  `70e6b89ab7925a58446d8c6bac32a32ac6dacf21cc7dd7567c31d6df1e56665d`).
- The complete Spec180 Python collection passes 95 tests with 19 warnings;
  the warnings are the existing Ultralytics/torch exporter diagnostics.
- The complete `./build/unit-tests --log_level=nothing` regression exits 0 and
  reports `*** No errors detected`; its output contains only existing bounded
  ONNX Runtime/provider diagnostic lines.
  the native `GenericDynamicApi/SelectionStrategies` suite passes 22 cases
  with no errors.

## Remaining blockers

The production `ProviderOfferV3` verifier must consume the projected evidence
and bind it to ACK name, Provider/service/request/attempt, canonical offer
digest, model/graph, validity window, boot epoch, and the candidate-bound
NDNSF Trust Schema anchor. Real malformed/mismatch negatives and a live
ACK→Selection→Provider→Response trace are still absent. T006, T009, T011, and
T014 therefore remain partial/open; no qualification result is claimed.
