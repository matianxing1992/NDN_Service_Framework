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

The build linked 127 tasks successfully in 4:41.392, with peak RSS
2,113,056 kB and zero swaps. The binary SHA-256 is:

```text
a2e92d09df0cbe60fd0a8a2d2f13119c565a007475f0fe6c38866be0490833ea
```

The binary loads Boost 1.71 from the system pair, NDN-CXX/NDN-SVS/NAC-ABE
from `/usr/local`, and ONNX Runtime from `/opt/onnxruntime`; `ldd` reported no
missing library and no `.local-boost171` library. The complete build and
identity records are retained in:

- `.codex-tmp/spec189-b189-2-placement-build-r1/integration-build.log`
- `.codex-tmp/spec189-b189-2-placement-build-r1/integration-identity.log`

## Selectors

All selectors were run from the repository root against the binary above.

| Selector | Result | Raw record |
| --- | --- | --- |
| `Spec170NdnsfDiCoreFlow/ProductionNativeHandlersRunD2bRequestToFinalResponse` | `PASS`, 1/1 | `.codex-tmp/spec189-b189-2-placement-build-r1/d2b-production.log` |
| `Spec170NdnsfDiCoreFlow/ProductionNativeHandlersPrepareRolesAfterSelection` | `PASS`, 1/1 | `.codex-tmp/spec189-b189-2-placement-build-r1/d2b-post-selection.log` |
| `Spec170NdnsfDiCoreFlow/ProductionNativeHandlersRejectTamperedD2bCapability` | `PASS`, 1/1 | `.codex-tmp/spec189-b189-2-placement-build-r1/d2b-tampered.log` |

The first invocation used an incomplete Boost.Test filter and selected no test
cases. It is retained as an invocation-boundary record in
`.codex-tmp/spec189-b189-2-placement-build-r1/production-selection.log` and
was not interpreted as a protocol result.

## Five-lane result

| Lane | Result | Boundary |
| --- | --- | --- |
| static | `INHERITED_STATIC_PASS` / no source change | The existing fixture's projection and handler assertions were inspected; this unit changed no production or test source, so its prior static review remains the source review. |
| compile-link | `PASS` | Full `integration-tests` source closure linked from global-r3. |
| runtime-test | `PASS` | Three named C++ selectors passed. |
| integration/wiring | `FOCUSED_PASS` | Production request → ACK → encrypted Selection → two handler entry paths were observed. |
| unobserved | `OPEN` | No direct unselected-Provider fetch/assembly counter, canonical Qwen manifest, real Repo layer publication, or MiniNDN path. |

## Closure decision

This evidence closes only the C++ production-ingress focused sub-check. It does
not close T005 or B189-2 because the current fixture does not directly record
that an unselected Provider performs zero layer fetches/assembly before
Selection, and it uses deterministic test runner material rather than the real
Qwen canonical manifest. The next bounded work must add those production
counters and candidate-bound manifest/Repo evidence before T005 can move beyond
`PARTIAL`.
