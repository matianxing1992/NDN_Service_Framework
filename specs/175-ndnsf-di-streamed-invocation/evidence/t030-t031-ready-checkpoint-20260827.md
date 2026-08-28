# T030/T031 readiness and checkpoint-binding checkpoint (2026-08-27)

This is implementation evidence only. It does not close the real
multi-process MiniNDN, CUDA, SIF, or Tiger acceptance gates.

## Corrections applied

- A committed Provider conversation entry now stores the exact aggregate
  checkpoint digest that authorized its promotion. A later Python acquire and
  native Selection resolution reject a valid receipt paired with a different
  checkpoint.
- The native Provider now publishes a compact, signed/encrypted
  `ndnsf-di-conversation-state-ready-v1` record after resolving the exact
  role-local parent state and before entering delta prefill. The record binds
  request, attempt, generation, plan, conversation/parent epoch, role,
  receipt, Provider boot/cache identity, readiness, and residency; it carries
  no model state bytes.
- The Python conversation owner waits for and validates one state-ready record
  from every selected role on resumed turns before committing the successor
  checkpoint. Initial full-context turns do not wait for a parent-state
  barrier.

## Verification

```text
./waf build --target=unit-tests -j1
build/unit-tests --log_level=message
  exit 0; no errors detected

build/integration-tests --run_test='Spec175InvocationStream*' \
  --log_level=message
  16 test cases; no errors detected

PYTHONPATH=pythonWrapper:NDNSF-DistributedInference \
  python3 -m pytest -q tests/python/test_spec175_*.py \
  tests/python/test_streamed_invocation_api.py
  189 passed
```

The focused source tests prove exact checkpoint/reference rejection and the
existing Provider receipt/control barrier. A real Provider-signed readiness
round trip and two-turn four-role MiniNDN run remain required by T030/T031,
T033, and T022.
