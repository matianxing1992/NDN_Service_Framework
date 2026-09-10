# R11-B8 C++ Prepared-Role Fixture Repair — 2026-09-10

## Scope

This is a bounded C++ test-fixture repair. It does not implement a maintained
caller and does not advance R11-B8, R11-B9, T010, T011, T012, T013, T016, or
the whole-Spec qualification gate.

## First boundary

The earlier broad `Spec182*` unit selector stopped in six sampling/epoch-text
fixtures with:

```text
std::invalid_argument: NativeProviderRuntime requires a runner preparation callback
```

The failure occurred before sampling assertions. `NativeEpochCoordinator`
executes the prepared-role seam, while `runSamplingEpochs` registered a runner
but supplied no preparation callback. The boundary was a test-fixture contract
gap, not a native protocol result. The original diagnostic remains in
`.codex-tmp/spec182-r11-b6-build/unit-selector.log`.

## Change

`tests/unit-tests/distributed-inference-async-runtime.t.cpp` now retains the
deterministic runner in a local `samplingRunner`, registers it with the runtime,
and sets `config.prepareRunner` to return that same runner. The fixture therefore
exercises the production prepared-role path without changing production code or
weakening the runtime guard.

## Static and build checks

- The requested cppcheck invocation reached its internal AST boundary and
  reported `internalAstError` at the fixture's existing `config` construction;
  this is recorded as a checker limitation, not a source compile result.
- System-first Waf build from `build-nac182`:

  ```text
  env PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin \
      CXX=/usr/bin/g++ CC=/usr/bin/gcc WAFLOCK=.lock-waf \
      ../waf build --targets=unit-tests -j2
  ```

  `190/190` build steps completed and `unit-tests` linked successfully in
  `5m57.842s`; raw output is `.codex-tmp/spec182-r11-b8-fixture-build.log`.

## C++ primary verification

The repaired fixture and neighboring stream contract were executed from the
freshly linked C++ binary:

- `Spec182EpochText/*`: 2 cases, exit 0; see
  `.codex-tmp/spec182-r11-b8-fixture-selector.log`.
- `Spec182StreamAcceptance/*`: 7 cases, exit 0; see
  `.codex-tmp/spec182-r11-b8-stream-selector.log`.
- `Spec182GenerationOptions/*`: 2 cases, exit 0; see
  `.codex-tmp/spec182-r11-b8-options-selector.log`.
- Fresh complete `Spec182*`: 256 cases and 7077 assertions, exit 0; see
  `.codex-tmp/spec182-r11-b8-full-unit.log`.

This complete selector is C++ unit regression evidence only; it does not close
the maintained-caller or final qualification gates.
Maintained caller migration, independent worker/process closure, no-Python
qualification, dependency closure, and T016/T017 remain open.
