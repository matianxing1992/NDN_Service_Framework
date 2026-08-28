# T015 model/task-first streamed coordinator (partial)

**Date**: 2026-08-23  
**Status**: partial; runtime evidence and process-level workload proof remain open

## Implemented

- `AutomaticPlanningCoordinator.request_streaming(...)` requires event,
  completion, and error callbacks, defaults to `StreamedInvocationOptions`,
  and forwards `generation_mode="TOKEN_STREAMING"` through one existing
  collaboration.
- `APPClient.request_streaming(...)` exposes the same model/task-first API
  without a Provider list. `AutomaticStreamingHandle` preserves the
  collaboration/request ID and exposes stream events and terminal state.
- The example user path consumes the streamed handle and records event/error
  counts instead of expanding the generation into per-token Requests.

## Verification

```text
PYTHONPATH=pythonWrapper:NDNSF-DistributedInference:NDNSF-DistributedRepo/pythonWrapper \
  python3 -m pytest -q \
    tests/python/test_streamed_invocation_api.py \
    tests/python/test_spec175_streamed_generation.py \
    tests/python/test_spec175_qwen_stateful_onnx.py \
    tests/python/test_spec175_qwen_generation.py \
    tests/python/test_spec175_contract_gate.py
29 passed in 4.51s
```

The full TTFT/ITL/component-span evidence schema, real workload process
delivery, and G1/G2 process matrix are not complete. This checkpoint does not
qualify MiniNDN, SIF, or Tiger.
