# R11-B6 Native Replacement Process

**Date**: 2026-09-10
**Status**: CLOSED_FOR_VALIDATION (bounded independent C++ replacement process; parent qualification remains open)
**Scope**: native requester replacement planning/fencing, an independent backup Provider, and the no-admitted-provider terminal boundary

## Ownership and validation boundary

The production replacement options are parsed by C++ `DI_NativeRequester` and carried through
the existing `StreamRequestOptions` contract. The requester/Core owns attempt selection,
replacement eligibility and result acceptance; the Provider owns the attempt-bound plan,
receipt and execution state. The Python standalone driver only creates isolated PIB/TPM/config
directories, starts the C++ authority/requester/Provider processes, stops the first Provider at
the selected interruption point, and checks process markers. It does not calculate a replacement
plan, stream result or recovery state.

## Independent replacement success

The checked positive run was:

```text
python3 tests/standalone/run-spec182-native-stream-process.py --replacement \
  --run-root /tmp/spec182-r11-b6-replacement-1789043834
```

The driver exited `0`. Requester A first authenticated its ACK, then was stopped before
execution. The requester switched to the independently configured Provider B and accepted the
C++ stream oracle `[4,5,6,7,8,9,10,2]`. Provider B's log contains the recovery request name
`/NDNSF/DI/REQUEST/82e3...-1/recovery/2`, `attemptId="attempt-2"`, grant verification,
`onnxruntime-cpu`, `executionCompleted="true"`, and the matching `profileAttemptEpoch="2"`.
Provider A has no execution-evidence marker. This demonstrates a real cross-process successor,
not a same-process callback replacement.

The normal stream regression also passed after the replacement changes:
`/tmp/spec182-r11-b6-stream-regression-1789044257`. The conversation and restart-rejection
regressions passed at `/tmp/spec182-r11-b6-conversation-regression-1789044272` and
`/tmp/spec182-r11-b6-recovery-regression-1789044293`.

## No-backup terminal failure

The checked negative run was:

```text
python3 tests/standalone/run-spec182-native-stream-process.py --replacement --replacement-no-backup \
  --run-root /tmp/spec182-r11-b6-no-backup-1789043895
```

The driver exited `0` after validating the expected negative. Requester A had authenticated its
ACK but no eligible backup existed; the requester terminated once with
`NATIVE_REQUEST_STAGE_FAILED boundary=ACK_CLOSED message=native request stage failed: DI_NATIVE_NO_ADMITTED_PROVIDER`.
There was no success marker and no Provider execution evidence. This preserves fail-closed
behavior instead of manufacturing a response after replacement planning fails.

## C++ fencing and batch checks

The C++ `Spec182StreamAcceptance` selector passed all seven fencing/acceptance cases and ended
with `*** No errors detected` (`.codex-tmp/spec182-r11-b6-build/stream-fencing-selector.log`).
The C++ integration selector
`Spec170NdnsfDiCoreFlow/Spec182*` passed 9/9 cases (`integration-selector.log`). The requester
and Provider targets rebuilt from the current source in 36.506 s; the unit/integration targets
rebuilt in 3m12.973 s, both with system-first `-j2` after the host swap evidence.

Static gates passed: official `$review-agent` read-only review found no qualifying actionable
finding for the changed diff, Python syntax compilation passed, and `git diff --check` passed.

The broad C++ `Spec182*` selector still has six unrelated fixture failures, all stopping at
`NativeProviderRuntime requires a runner preparation callback` in the sampling/epoch-text
fixtures. The raw result is `.codex-tmp/spec182-r11-b6-build/unit-selector.log`; it is recorded
as a repository fixture boundary and is not promoted to a replacement failure.

This closes only R11-B6's bounded native replacement process evidence. It does not close T010,
T011, T013, R11-B7--B9, no-Python operation, or whole-Spec qualification.
