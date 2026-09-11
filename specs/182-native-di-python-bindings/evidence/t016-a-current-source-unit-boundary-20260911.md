# T016-A Current-Source C++ Unit Boundary — 2026-09-11

## Result

The first full C++ unit qualification attempt used the existing
`build-nac182/unit-tests` binary and exited `rc=201` after about 330 seconds.
The report contained 1,022 total cases, 11 failed cases, 8 aborted cases, and
20 failed assertions. The first boundary was
`NativeProviderRuntime requires a runner preparation callback` in the
conversation and opaque-state tests. The complete raw run is retained under
`.codex-tmp/spec182-t016-unit-20260911/`.

## Boundary classification

This binary was older than the current `runSamplingEpochs` fixture repair.
The current source tree had no C++ source diff after the native checkpoint, but
the existing binary was not rebuilt from the current fixture closure. The run
is therefore a stale-artifact failure and is not a current protocol result.
The changed gate is a fresh configure/build in a new output directory followed
by the complete C++ unit selector. T016-A remains `PARTIAL` until that gate and
the remaining native qualification matrix pass.

The fresh current-source configure then hit a separate target-selection
boundary: the first build command requested `DI_NativeRequester` and
`di-native-provider` without enabling examples. Waf stopped before any compile
with `Could not find a task generator for the name 'DI_NativeRequester'`. The
raw output is in `.codex-tmp/spec182-t016-unit-20260911/build.log`; this is not
a C++ product failure. The retry is split into the configured test targets and
an examples-enabled closure.

## Current-source retry

The fresh build completed all 309 Waf tasks with `-j4`, but the rebuilt
`unit-tests` selector exited `rc=201`: 1,026 total cases, 11 failed cases, 8
aborted cases, and 20 failed assertions. The first failures again require a
`runner preparation callback` in `distributed-inference-async-runtime.t.cpp`.
The raw output is retained at `.codex-tmp/spec182-t016-unit-20260911/unit-tests.log`.
This result is current-source evidence and supersedes the stale-artifact
classification for the selector; T016-A remains `PARTIAL` pending source-level
fixture repair and fresh reruns.

## Delayed-runner capability finding

The repaired fixture compiled, and the fresh full selector reduced the result
to one failure in `NativeProviderRuntimeCarriesOpaqueStateHandleWithoutHostRoundTrip`:
`coordinatorShared->sawCompactState` was false. The raw output is retained at
`.codex-tmp/spec182-t016-unit-20260911/unit-tests-final.log`. The production
cause was a capability probe performed before a delayed `prepareRunner` callback
created the runner. The bounded repair transports the resolved capability in
`ProviderRoleResult` and performs opaque-state finalization after the worker
returns, preserving admission-time preparation and Provider-local state.

## Final unit recheck

After the production fix and fixture callback repair, the fresh incremental
build completed 309/309 Waf tasks with system-first `-j4` in 127.78 seconds.
The affected coordinator selector passed 11/11 cases and 155/155 assertions.
The complete current-source unit selector then passed 1,026/1,026 cases and
71,011/71,011 assertions in 319.73 seconds. The unit binary SHA-256 is
`f75d7adbaa8250b6127969434512f305c9878af1bfb45476d6bdc8d9476b00d9`; its
RUNPATH is `/home/tianxing/NDN/nac-abe-integration-182/install/lib:/home/tianxing/NDN/ndn-svs/build`.
The source fix is checkpoint `45dd9f2f`. This closes the current-source C++
unit lane only; integration, no-Python, MiniNDN and final T016 gates remain
separate.

The first incremental compile of that repair failed because two similar patch
contexts added `config.prepareRunner` without adding the corresponding local
runner in the target function. The compiler stopped on the undeclared
`runner`; raw output is retained at
`.codex-tmp/spec182-t016-unit-20260911/fixture-repair-build.log`. This is a
test-only patch-context boundary and is repaired before rerunning selectors.

## Validation matrix

| Lane | Required result |
| --- | --- |
| Current-source build | New build directory, system-first toolchain, `-j4`, explicit NAC-ABE/ONNX/NDN-SVS prefixes |
| C++ unit suite | Fresh `unit-tests --report_level=short`, with case/assertion counts and binary hash |
| C++ integration suite | Fresh `integration-tests --report_level=short`, with first failure boundary retained |
| Native closure | `ldd`/RUNPATH and target identity for unit, integration, requester, and Provider binaries |
| T016 qualification | MiniNDN/no-Python/PO matrix remains a separate gate and is not implied by unit or integration PASS |

## Batch retrospective

The stale binary was discoverable from its modification time and the missing
fixture symbol in the failure text. Future full-suite attempts must record the
source commit, build directory, target binary hash, and fixture-repair commit
before executing the selector.

## Next step

Rebuild the current source closure in a fresh directory with the host's
authorized `-j4` setting, then run unit and integration selectors separately.
