# T009 Python/native streamed invocation evidence

Date: 2026-08-23 17:27 CDT

## Scope

This evidence closes only the generic Python binding boundary. It does not
claim the Spec175 automatic model/task-first DI path, stateful Qwen3.6, G2, or
any SIF/Tiger gate.

## Build identity

- Framework build: `build/libndn-service-framework.so.0.1.0`
- Controller executable: `build/examples/App_ServiceController`
- Host extension: `pythonWrapper/ndnsf/_ndnsf.cpython-38-x86_64-linux-gnu.so`
- Boost: 1.71 (`build/c4che/_cache.py`)
- NDN-SVS source/build: `/home/tianxing/NDN/ndn-svs`
- Extension SHA-256: `32b776fbaa7d6e02f683dd0d712eb45b8294c87c671f2712e53eb82abe0d1801`
- Framework SHA-256: `53465911983621b91d503c6a0e7ed068f02604e1e6b41999f53a305edc125835`
- Controller SHA-256: `c33bc65bf96f0de9c10ae72244ec62cb7cf3d144c0f0deca9faa58e55e8f1db1`
- `ldd` checks for the extension and controller had no `not found` entries.

## Real process command

```text
bash examples/run_python_streamed_invocation_regression.sh
```

The runner starts a real NFD (or validates an existing socket with `nfdc
status`), ServiceController, Python Provider, and Python User. The corrected
runner rejects stale `pgrep nfd` state instead of treating a missing
`/run/nfd/nfd.sock` as a healthy forwarder.

## Observed result

```text
PYTHON_STREAM_PROVIDER_FINISHED
PYTHON_STREAM_WRITER_FENCED=PASS
PYTHON_STREAM_USER_PASS request_id=<fresh> events=5 callback_events=5 contained_failure=1 result=complete-result
PYTHON_STREAMED_INVOCATION_REGRESSION=PASS
```

The User consumed five ordered events through the async iterator and one final
result, then repeated the same Core path through callbacks. A contained Python
handler exception reached the error callback/result without escaping the
process. The post-return writer probe rejected late event, terminal, and error
publication. Provider logs show real Request/ACK/Selection/Response packets;
the test does not use a Python-only cursor queue.

## Boundary

The result proves the native `request_service_streaming_handle`, callback vs
iterator single-consumer rule, and Core-owned Provider writer. It does not
prove `AutomaticPlanningCoordinator.request_streaming(...)` driving a native
DI Provider; that remains T015/G2 work.
