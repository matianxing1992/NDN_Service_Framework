# Quickstart Validation Guide

This guide validates the implemented facade.

## Contract gate

1. Confirm every signature and default matches
   [contracts/high-level-api.md](contracts/high-level-api.md).
2. Compare the four before/after pairs in
   [contracts/before-after.md](contracts/before-after.md).
3. Confirm bootstrap follows `announce -> publish -> activate`.
4. Reject any implementation that removes explicit `announce()` for later
   future samples.

## Focused implementation gate

Validated commands:

```bash
./waf build -j2
./build/unit-tests --run_test=StreamFacade
python3 tests/python/test_ndnsf_stream_facade.py
```

Expected result:

- C++ and Python facade contracts match;
- derived definitions and names match golden vectors;
- Provider and consumer delegates run exactly once;
- invalid lifecycle operations fail without partial publication.

## Regression gate

```bash
./build/unit-tests --run_test=Stream
./build/unit-tests --run_test=EncryptedPermissionResponse
python3 tests/python/test_ndnsf_core_streaming.py
python3 tests/python/test_ndnsf_live_stream_generality.py
python3 tests/python/test_spec146_acoustic_stability.py
```

Existing low-level tests must remain unchanged and pass.

## Audit gate

- Use CodeGraph to inspect every new facade caller/callee.
- Verify no adaptive-fetcher, Mapping, FEC, retry, timeout, Nack, or recovery
  algorithm hunk exists.
- Verify Spec 144/146 canonical result hashes.
- Compare low-level and facade packet/name/descriptor/status outputs.
