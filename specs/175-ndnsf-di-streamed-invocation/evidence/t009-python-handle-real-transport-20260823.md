# T009 host Python handle and real transport evidence — 2026-08-23

Status: `HOST_PASS`; the exact CPython-3.10 candidate-SIF build remains part of
the later immutable-image gate and is not claimed here.

## Implemented public contract

- `ServiceUser.request_service_streaming(...)` returns one
  `StreamedInvocation` with immutable request ID, Core status/metrics,
  `async for`, awaitable `result()`/idempotent `cancel()`, decoders, callbacks,
  and the callback-versus-iterator single-consumer guard.
- Normal accepts no Provider target; Targeted requires exactly one target.
- Python Providers register plain or authenticated-context streamed handlers
  and receive only the Core-owned writer capability.
- `max_events` is the total cursor budget including End. Five application
  events therefore use `max_events=6`.
- The Python error-code boundary now accepts the complete Core enum through
  `ReplacementUnavailable`, instead of the stale `1..8` subset.

## Build and ABI closure

The first real run exposed that rebuilding only `_ndnsf` had left the older
08:38 framework library underneath the new binding. The framework was rebuilt
with the configured Waf toolchain before relinking the extension:

```text
./waf build --target=ndn-service-framework -j2             PASS
cd pythonWrapper && python3 setup.py build_ext --inplace   PASS
python=3.8.10
native=pythonWrapper/ndnsf/_ndnsf.cpython-38-x86_64-linux-gnu.so
framework sha256=53465911983621b91d503c6a0e7ed068f02604e1e6b41999f53a305edc125835
extension sha256=32b776fbaa7d6e02f683dd0d712eb45b8294c87c671f2712e53eb82abe0d1801
```

`ldd` resolves the current local framework and NDN-SVS libraries, ndn-cxx
0.9.0, and Boost 1.71 chrono/date_time/log/thread/stacktrace with no `not
found`. The stale untracked CPython-3.10 extension from August 17 was moved to
the system Trash so a later SIF build cannot select it by wildcard. The SIF
must build a fresh CPython-3.10 extension inside its own ABI boundary.

## Tests

```text
PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper:\
NDNSF-DistributedInference:Experiments \
python3 -m pytest -q tests/python/test_streamed_invocation_api.py
21 passed

examples/run_python_streamed_invocation_regression.sh
PYTHON_STREAM_WRITER_FENCED=PASS
PYTHON_STREAM_ERROR_CALLBACK code=17
PYTHON_STREAM_USER_PASS ... events=5 callback_events=5 contained_failure=1
PYTHON_STREAMED_INVOCATION_REGRESSION=PASS

./build/unit-tests \
  -t Spec175InvocationStreamLifecycle/\
StreamEventPublisherEncryptsSignsRetainsAndRepublishesExactWire
No errors detected
```

The real regression starts NFD, the native ServiceController, one Python
Provider, and one Python User. It proves actual Request/ACK/Selection, five
encrypted signed event fetches in exact order, End plus terminal Response,
both iterator and callback consumption, callback/iterator exclusion, Provider
exception containment, worker-thread Provider dispatch, and rejection of
publish/finish/fail through a retained writer after its handler returns.

## Remaining T009 boundary

Do not mark all of T009 complete until the same surface, real regression, ABI
closure, and import checks pass inside the one promoted CPython-3.10 SIF.
