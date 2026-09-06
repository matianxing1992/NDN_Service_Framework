# T011 Native Boundary Repair Evidence — Revision 123

**Status**: implementation evidence only; T011 and T014 remain open.

This checkpoint records the focused repair after the ACK disposition blocker
was traced through the actual native Provider path. It does not replace the
required real-NFD Y-N execution, local qualification inventory, candidate
seal, SIF replay, or Tiger evidence.

## Repairs covered

- `NativeProviderHandler` now binds the authenticated application input to
  each declared `APPLICATION_INPUT` edge in the selected V3 role and
  pre-satisfies that edge with the request-backed encrypted input object.
  Non-ingress roles do not fetch application input. A Y-N-I mutation attempts
  the non-ingress fetch at the native Provider boundary and fails closed with
  `DI_INPUT_FETCH_ROLE_MISMATCH` and the exact negative marker.
- The runner passes `SPEC180_YN_MUTATION` to the native Provider process. The
  Y-N-C mutation removes `FullModel` and `Merge` from the real native process
  capability vector, so the negative is not only a Python profile mutation.
- The live-case negative path uses the same child-process driver for
  Y-N-C/P/R/I/E/L, waits for the exact boundary marker, stops the case-owned
  runtime, verifies all retained children have exited, and records the
  subcase result only after that cleanup check.
- The normal production V3 planner keeps artifact/runtime grant protection
  separate from the application-input encryption epoch. The native protected
  grant factory and live stale/revoked-epoch path remain open T010/T014 work;
  no synthetic grant or self-authenticating substitute was introduced.

## Focused verification

From the repository root:

```text
python3 -m pytest -q --tb=short \
  tests/python/test_spec180_yolo_application.py \
  tests/python/test_spec180_yolo_minindn.py \
  tests/python/test_spec180_yolo_security.py \
  tests/python/test_spec180_generic_request_api.py \
  tests/python/test_spec180_role_assembly.py \
  tests/python/test_spec180_yolo_equivalence.py \
  tests/python/test_spec180_terminal_collector.py \
  tests/python/test_spec180_tiger_contract.py \
  tests/python/test_spec180_yolo_numerical.py \
  tests/python/test_spec180_native_evidence.py
```

Result: **180 passed** (`11.98s`). `git diff --check` passed. The repository
build `./waf -o build-system-j2 build -j2` also completed **396/396**
successfully. The current `NativeProviderHandler.cpp` and
`DI_NativeProviderExecutable.cpp` therefore pass both the C++17 compile/link
path and the standalone `-fsyntax-only` command with the existing NDNSF,
NAC-ABE, NDN-SVS, build, and ONNX Runtime include paths. The build emitted
only existing unused-function warnings in the native example translation
unit.

The existing `--with-tests` build also compiled the new
`distributed-inference-native-plan.t.cpp` translation unit, but the aggregate
`unit-tests` target could not link/finish because the pre-existing
`ServiceProvider.cpp:5665` path calls `subscribeToProducerWithCatchUp`, which
is absent from the configured NDN-SVS headers. No NDN-SVS or unrelated source
was changed to mask that dependency mismatch, and the aggregate C++ unit
binary was not claimed as executed.

## Evidence boundary and next gate

No new SIF build, Tiger submission, or SIF/Tiger experiment was performed.
These checks prove source-level wiring and focused fail-closed behavior only;
they do not prove live NFD/NDN-SVS encrypted fetch, ACK/Selection closure,
native Merge execution, CUDA/device identity, or candidate-bound cleanup.
The next gate is a fresh T014 design-code convergence audit after the current
source/runtime/harness binding is closed; formal T015 and all SIF/Tiger work
remain paused until that gate passes.
