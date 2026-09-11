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
