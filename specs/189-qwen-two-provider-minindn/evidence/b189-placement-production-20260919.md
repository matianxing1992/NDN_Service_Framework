# B189-2 Production Placement Ingress Evidence

**Date**: 2026-09-19
**Status**: `FOCUSED_CXX_PASS` / `T005 PARTIAL` / `B189-2 NOT_ACCEPTED`

## Scope

This run exercised the existing C++ production `Spec170NdnsfDiCoreFlow`
fixture after the protected Repo boundary checkpoint. It creates two Providers,
sends a request through the production request publisher, publishes ACK and
encrypted Selection through the in-process SVS boundaries, and enters two
`NativeProviderHandler` instances. The post-Selection case uses the production
`runnerPreparationFactory` seam and checks that the projection is bound to the
local Provider, request, artifact and role before runner creation. The negative
case changes the request-scoped projected capability and requires the Provider
lifecycle to fail without a response.

This is a production-ingress focused gate. It is not a real Qwen candidate or a
MiniNDN qualification run.

## Build and binary identity

The complete `integration-tests` target was built from the existing global-r3
Waf tree with the repository's system-first toolchain:

```text
/usr/bin/python3 ../waf build --targets=integration-tests -j4
```

The affected `integration-tests` target rebuilt successfully in 39.072 seconds
with `-j4`, peak RSS 2,114,188 kB and zero swaps. The binary SHA-256 is:

```text
4013074f8d6e536cf99847db0c2de3ca0101dd21809c1ed2027ed038cf7050f6
```

The binary loads Boost 1.71 from the system pair, NDN-CXX/NDN-SVS/NAC-ABE
from `/usr/local`, and ONNX Runtime from `/opt/onnxruntime`; `ldd` reported no
missing library and no `.local-boost171` library. The complete build and
identity records are retained in:

- `.codex-tmp/spec189-b189-2-preselection-build-r1/integration-build-r2.log`
- `.codex-tmp/spec189-b189-2-preselection-build-r1/integration-identity-r2.log`

## Selectors

All selectors were run from the repository root against the binary above.

| Selector | Result | Raw record |
| --- | --- | --- |
| `Spec170NdnsfDiCoreFlow/ProductionNativeHandlersRunD2bRequestToFinalResponse` | `PASS`, 1/1 | `.codex-tmp/spec189-b189-2-preselection-build-r1/d2b-production-r2.log` |
| `Spec170NdnsfDiCoreFlow/ProductionNativeHandlersPrepareRolesAfterSelection` | `PASS`, 1/1; both preparation counters were `0` before the manual ACK/Selection publication and then reached provider0=`2`, provider1=`1` after Selection | `.codex-tmp/spec189-b189-2-preselection-build-r1/d2b-post-selection-r2.log` |
| `Spec170NdnsfDiCoreFlow/ProductionNativeHandlersRejectTamperedD2bCapability` | `PASS`, 1/1 | `.codex-tmp/spec189-b189-2-preselection-build-r1/d2b-tampered-r2.log` |

The first invocation used an incomplete Boost.Test filter and selected no test
cases. A later recording attempt also hit a missing run-directory boundary;
both are retained as invocation-boundary records in
`.codex-tmp/spec189-b189-2-placement-build-r1/production-selection.log` and
`.codex-tmp/spec189-b189-2-preselection-build-r1/selector-invocation-boundary-r1.log`.
Neither was interpreted as a protocol result.

## Five-lane result

| Lane | Result | Boundary |
| --- | --- | --- |
| static | `STATIC_PASS` | The new pre-Selection counter assertion received official read-only review; frozen diff SHA-256 `d177bf5740a55520a4ac4445590db4beb52cfc8801e5b9125b45d3d580e907ce`, no findings. |
| compile-link | `PASS` | Full `integration-tests` source closure linked from global-r3. |
| runtime-test | `PASS` | Three named C++ selectors passed. |
| integration/wiring | `FOCUSED_PASS` | Production request → ACK → encrypted Selection → two handler entry paths were observed; post-Selection preparation was zero before ACK and positive after Selection. |
| unobserved | `OPEN` | No direct unselected-Provider fetch/assembly counter, canonical Qwen manifest, real Repo layer publication, or MiniNDN path. |

## Closure decision

This evidence closes only the C++ production-ingress focused sub-check. It does
not close T005 or B189-2 because the current fixture does not directly record
that an unselected Provider performs zero layer fetches/assembly before
Selection, and it uses deterministic test runner material rather than the real
Qwen canonical manifest. The next bounded work must add those production
counters and candidate-bound manifest/Repo evidence before T005 can move beyond
`PARTIAL`.
