# Spec175 full source-tree regression refresh — 2026-08-23

> Historical snapshot superseded by `evidence/g2-live-i13-20260823.md` and the
> current 67/67 integration run. The earlier 57/57 and 24-process counts are
> retained here for audit history.

This is a development-tree regression checkpoint after the r27 SIF source
closure fixes. It is not a sealed promotion manifest.

## Results

```text
./build/unit-tests --log_level=message
  PASS: 567/567

./build/integration-tests --log_level=message
  PASS: 57/57

Spec175 Python focused inventory
  PASS: 86 passed, 1 skipped
```

The native binaries were unchanged during the refresh:

```text
unit-tests:       e067b97c398884720070c020a1b8b6d2a7100e4c609bbc3ed3f8f8e4d4dc1fdf
integration-tests: d5ffc604a6d3abda545b6a4bf7a9e55663b1516930bde8a2bcae157333057ea5
```

## Gate boundary

G0 remains `BLOCKED_EXPECTED` because the shared worktree has 161 in-scope
dirty paths and has not been sealed. G2 remains
`BLOCKED_MISSING_CASES`: all 24 registered processes pass, I12 is not
registered, and I13 is only the deterministic no-replacement lower-bound
oracle rather than a live Provider-loss boundary. No G3–G7 or Tiger claim is
derived from this regression.

## Interpretation

The refresh confirms that the source-closure edits for
`InvocationStream.cpp` and `NativeEpochCoordinator.cpp` did not regress the
existing native suites. It does not substitute for the missing automatic-DI
Python fresh-process proof, opt-in native I12 recovery, or the full local/Tiger
qualification ladder.
