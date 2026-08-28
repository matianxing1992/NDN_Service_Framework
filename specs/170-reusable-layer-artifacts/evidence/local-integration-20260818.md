# Spec 170 local integrated regression — 2026-08-18

This is a real compiled integration run, not a fixture-only Python check.
The executable uses the repository's `DummyClientFace`/SVS integration
environment and the production NDNSF-DI ingress/native-handler path.

## Command and immutable outputs

```text
./build/integration-tests --log_level=test_suite
log: /tmp/spec170-integration-tests-20260818.log
log sha256: bbbaca0b26f187161cd05ccf5caa3814170c31ef3295a2ca75284f5338bb8306
integration executable sha256: d362d4eb185b9ec65b4d8488d5258b283f59662783eeaaa52c0e6796ac464cff
```

The run reported `Running 29 test cases...` and ended with
`*** No errors detected` (Boost.Test, approximately 18.31 s).
The concise rerun independently reported `29 test cases out of 29 passed` and
`427 assertions out of 427 passed`; its log SHA-256 is
`1441d2ce6cb334873752b130365df638f10795dc3a4caded120a99d7c199ae3f`.

## Coverage observed

- NDN-SVS request publication and service response delivery.
- `NDNSF_DATA_V1` SVS mapping/repair/replay-fence flow and production
  Provider collaboration segments.
- Production post-Selection assignment fetch, native handler dispatch, and
  final Response.
- Two-device local execution, D2b Selection-to-SVS data flow, and native D2b
  request-to-Response.
- Native heterogeneous mappings `[1,2,1]` and `[2,1,2]`, including complete
  oracle responses.
- Four-Provider role split and same-Provider multi-role collaboration.
- Missing Backbone output: dependency cancellation is observed before the
  global request deadline; no partial downstream Response is accepted.
- Preconfigured environment bootstrap/READY separation, deterministic packet
  faults, generic request lifecycle, and custom three-Provider selection.

This proves the local integrated control/data paths for the listed cases. It
does not prove a real model run, TigerCluster execution, or the full frozen
Spec170 Gate-A/security corpus by itself.

## Python Spec170 contract lane

After the integration run, the full Python lane was run with the repository's
required import roots (including the DistributedRepo binding):

```text
PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference \
  python3 -m pytest -q tests/python/test_spec170_*.py
98 passed, 9 skipped, 1 warning in 9.85s
log sha256: bf9c4907abe74f206e7554b49c5b075085a8bc894f30f41c54b7d38164b1f7d2
```

The only failure in the first correctly imported run was a stale test double
missing the production `Invocation.request_id` field. The fixture was aligned
with the production object, the focused file passed `10/10`, and the complete
lane then passed. The nine skips remain explicit real-environment/model gates;
they are not counted as passes.
