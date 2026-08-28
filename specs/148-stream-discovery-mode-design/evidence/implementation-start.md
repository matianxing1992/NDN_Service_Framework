# Spec 148 Implementation Start Evidence

**Date**: 2026-07-25  
**Status**: migration started; feature not closed

## Completed boundary work

- The C++ high-level `StreamPublisher` surface now contains only
  `start()`, `push()`, `flush()`, `status()`, and `stop()`.
- The native binding and Python high-level facade no longer expose
  `announce`, `publish`, `start_predictive`, or the old bootstrapping
  `start(...)` overload.
- Compile-time C++ detection tests distinguish the removed high-level methods
  from the intentionally retained internal `LiveStreamPublisher` primitives.
- Python attribute tests prove the removed methods are absent.
- The high-level provider now selects a predictive-only Core route lifecycle
  instead of trying to activate an empty Mapping-first publisher.
- A first exact-wire Core primitive retains the supplied `ndn::Data` object and
  can match a pending canonical predictive-name Interest without wrapping or
  re-signing it.

## Verification

```text
./waf build -j2 --targets=unit-tests
  PASS

./build/unit-tests --run_test=StreamPredictive
  PASS: 4/4 cases, 12/12 assertions

./build/unit-tests --run_test=StreamFacade
  PASS: 5/5 cases, 21/21 assertions

./waf build -j2 --targets=ndn-service-framework
  PASS

CFLAGS='-O0 -g0' CXXFLAGS='-O0 -g0' \
  python3 setup.py build_ext --inplace --force
  PASS

PYTHONPATH=pythonWrapper \
  python3 tests/python/test_ndnsf_stream_facade.py
  PASS: 6/6
```

The first default-optimization Python binding rebuild was killed by the kernel
while compiling the large pybind11 translation unit. The retained retry used
`-O0 -g0` to reduce build memory and completed successfully. `ldd` resolves the
rebuilt extension to this checkout's
`build/libndn-service-framework.so.0.1.0` and Boost 1.71 libraries.

The source scan still finds `.def("publish", ...)` for the intentionally
retained internal `LiveStreamPublisher` binding and an unrelated collaboration
context. Neither is the removed high-level `StreamPublisher` facade.

## Explicitly incomplete

- The authenticated frontier and group metadata route is not implemented.
- `flush()` does not yet provide unequal-length-safe repair metadata or atomic
  frontier advancement.
- Provider-side cryptographic validation, all negative/error cases, and
  concurrency fencing are incomplete.
- `PredictiveStreamSubscriber` still needs validator integration, adaptive
  scheduling reuse, bounded recovery, and complete metrics.
- UAV Drone/Ground Station are not yet migrated to a single predictive path.
- No Spec 148 MiniNDN smoke or formal cell has been run.

Therefore only T001, T002, and T004 are complete. Build/test success here is
implementation-start evidence, not Spec 148 acceptance or closure.
