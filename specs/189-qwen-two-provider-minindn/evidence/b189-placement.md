# B189-2 Placement Evidence

**Status**: EXPLORATORY_PARTIAL / NOT_ACCEPTED

Real MiniNDN runs r04-r21 reached `ACK_CLOSED` and committed a signed
two-provider Selection. The requester logs show `Runtime.open → User.prepare →
PreparedModel.request`, and r21 records two `NDNSF_COLLAB_ASSIGNMENT_SELECTED`
entries followed by `NDNSF_DI_NATIVE_SELECTION_COMMITTED`. This is exploratory
runtime evidence only: the named C++ placement oracle has not run, the
request-envelope payload-free assertion has not run, and no-fetch-before-
Selection counters have not run. It therefore does not close B189-2.

The required acceptance order remains
`REQUEST_REFERENCE_ONLY → ACK → SELECTION_2_PROVIDERS`, with no layer fetch or
runner creation before Selection.

## Five-lane coverage

| Lane | State | Evidence / gap |
| --- | --- | --- |
| production entry/callers | `covered-partial` | real requester and Core ACK/Selection path observed in r21 |
| implementation/wire | `covered-partial` | signed assignments and Selection marker observed; payload parser and placement binding oracle absent |
| test/harness/oracle | `covered-partial` | `spec189-request-wire` now proves the reference-only envelope and negative wire cases; ACK/Selection and no-fetch oracle remain absent |
| build/source closure | `covered-partial` | `tests/wscript` registers the wire-only selector; complete placement source map remains open |
| migration/evidence | `covered-partial` | r04-r21 logs retained; acceptance and repeat remain open |

## Closure decision

`OPEN_FOR_NEXT_BATCH` with trigger: register and run the C++ request/placement
oracle, then preserve the no-fetch-before-Selection result. Existing ACK and
Selection observations do not count as task completion.

## T004 wire-only request gate — 2026-09-18 11:35 -0500

The new C++ target `spec189-request-wire` calls the production
`encodeNativeRequestEnvelope` and passed three cases:

| Case | Result |
| --- | --- |
| repeated request envelope | stable model reference with all namespace/name/digest/size/epoch/scope fields; no model payload or URL; request and invocation identities differ |
| arbitrary URL / invalid reference | rejected before encoding |
| oversized inline payload | 16 MiB limit rejected before wire construction |

Build used the existing globally configured tree with `-j4` and completed in
16.778 seconds; logs are
`.codex-tmp/spec189-t004-request-wire-build.log` and
`.codex-tmp/spec189-t004-request-wire-selector.log`. The frozen static review
snapshot `.codex-tmp/spec189-t004-request-review-r2/diff.patch` (SHA-256
`8a074fa991d7aa35a7dc2ed64aa2510162c8f8863cdf8d483c3e946d7a0e7ef0`) received
`STATIC_PASS` with no P0/P1/P2.

This is deliberately a **wire-only** C++ gate. It does not exercise
`PreparedModel::request`, a prepared lease, Repo publication counters, or the
real two-provider ACK/Selection path. T004 is therefore `PARTIAL`, and B189-2
remains `NOT_ACCEPTED` until the production-handle and placement evidence run.

## T004 production PreparedModel gate — 2026-09-18

The production-handle selector `spec189-prepared-request` now uses the
maintained `Runtime::open → bindInProcessRuntime → User::prepare →
PreparedModel::request` path. The selector injects the concrete
`RepoSourceProvider` through both source-provider and artifact-publisher
interfaces, so preparation performs one cold source lookup, one fallback
ingest, and one durable publication into a private filesystem Repo. It then
submits two requests from the same prepared handle and checks that both IDs are
distinct while `lookups`, `missIngests`, and `publicationCalls` remain
unchanged. The Repo source is present before either request. Runtime close and
drain complete with the fixture's observed cancellation/failed terminal state.

The frozen review snapshot after the macro-visibility, self-contained-fixture,
and private-directory fixes received `STATIC_PASS` in review r16; compile-link and runtime evidence
are separate. The target linked the complete DI source closure and
`ndnsf-distributed-repo` with the existing global dependency tree. The final
incremental build completed in 28.394 seconds (`.codex-tmp/spec189-t004-
prepared-build-r12.log`), and the C++ selector passed in 0.643 seconds with no
test errors (`.codex-tmp/spec189-t004-prepared-selector-r11.log`). The selector
also rejects a request submitted through the closed prepared client with
`CLIENT_CLOSED` at the request boundary.

This closes the production PreparedModel/Repo reuse sub-check for T004. It
does not prove stale-manifest negatives, real Core ACK/Selection, or
the two-provider no-fetch-before-Selection boundary; T004 and B189-2 remain
`PARTIAL`/`NOT_ACCEPTED`.

## T005 C++ placement and cache-layer gate — 2026-09-18

The new target `spec189-placement-oracle` uses the production
`NativePreSplitFirstPlacement::proposeRoles`,
`validateNativeRolePlacement`, and `ProviderArtifactCache` implementations.
The fixture is derived from the signed V3 offer oracle and supplies two
rank-cover roles. The selector verifies that placement assigns distinct
Providers, preserves the selected layer ranges and artifact identities, and
passes the production placement validator. It then submits a non-empty but
incomplete grant-bound projection and confirms that the cache rejects it before
the builder (`builds == 0`). Complete projections for the two selected
Providers each build one exact layer and release a valid cache lease.

The static review used the immutable v5 snapshot
`.codex-tmp/spec189-t005-placement-review-v5/patch.diff`, SHA-256
`a73c81455ac92175345f9a19698369c189cfa2683c2c25c5e65fcdba25ea0dce`; the
official read-only review-agent returned `STATIC_PASS` with no P0/P1/P2. The
first build invocation from the repository root selected the wrong locked Waf
tree and is recorded in `b189-build-20260918.md` and `docs/failure-log.md`; no
compile was accepted from that attempt. The retry from
`build-spec189-b189-3-global-r3` with `-j4` compiled and linked 108 tasks in
128.435 seconds. The focused C++ selector passed in 17.213 ms with no errors.
Raw logs are `.codex-tmp/spec189-t005-placement-build-r2.log` and
`.codex-tmp/spec189-t005-placement-run-r1.log`.

| Lane | Result |
| --- | --- |
| static | Historical v5 `STATIC_PASS`, corrected by the architecture audit: role identity and validator coverage exist, but synthetic grant naming is not canonical production grant verification; affected fixture remains open in T005 |
| compile-link | `PASS`; registered target built from the existing global-r3 tree with the complete DI integration source closure and `-j4` |
| runtime-test | `PASS`; `Spec189PlacementOracle/TwoProviderSelectionKeepsPreSelectionEffectsZero` passed as a C++ selector |
| integration/wiring | `PASS`; `tests/wscript` registers the target and the selector uses the signed offer fixture, production placement and cache APIs |
| unobserved | real Core ACK/Selection, parser/ProtectedRuntime/Provider admission, and network no-fetch-before-Selection remain unobserved |

This is a focused placement/cache-layer result, not full T005 completion. The
latest r25 logs include execution entry and Provider-0 assembly entry, but no
runner/output completion. T005 and B189-2 remain `PARTIAL`/`NOT_ACCEPTED`.

## T005 production C++ ingress focused gate — 2026-09-19

The existing production `Spec170NdnsfDiCoreFlow` fixture was rebuilt from the
global-r3 Waf tree and exercised through the repository-root binary. It creates
two Providers, sends a real request through the production request publisher,
publishes ACK and encrypted Selection through the in-process SVS boundaries,
and enters two `NativeProviderHandler` instances. The post-Selection variant
uses the production `runnerPreparationFactory` seam and checks that the
projection is bound to the local Provider, request, artifact and role before
the runner is created. The tamper variant changes the request-scoped projected
capability and requires the Provider lifecycle to fail without a response.

| Selector | Result | Evidence |
| --- | --- | --- |
| `Spec170NdnsfDiCoreFlow/ProductionNativeHandlersRunD2bRequestToFinalResponse` | `PASS`, 1/1 | [d2b-production.log](../../../.codex-tmp/spec189-b189-2-placement-build-r1/d2b-production.log) |
| `Spec170NdnsfDiCoreFlow/ProductionNativeHandlersPrepareRolesAfterSelection` | `PASS`, 1/1 | [d2b-post-selection.log](../../../.codex-tmp/spec189-b189-2-placement-build-r1/d2b-post-selection.log) |
| `Spec170NdnsfDiCoreFlow/ProductionNativeHandlersRejectTamperedD2bCapability` | `PASS`, 1/1 | [d2b-tampered.log](../../../.codex-tmp/spec189-b189-2-placement-build-r1/d2b-tampered.log) |

The complete `integration-tests` target linked successfully with the existing
NDNSF source closure: 127 tasks, `-j4`, 4:41.392 wall time, peak RSS
2,113,056 kB and zero swaps. The tested binary SHA-256 is
`a2e92d09df0cbe60fd0a8a2d2f13119c565a007475f0fe6c38866be0490833ea`.
`ldd` resolves Boost 1.71 from the system pair, NDN-CXX/NDN-SVS/NAC-ABE from
`/usr/local`, and ONNX Runtime from `/opt/onnxruntime`; no `.local-boost171`
library was loaded. The build log and identity record are
[integration build](../../../.codex-tmp/spec189-b189-2-placement-build-r1/integration-build.log)
and [integration identity](../../../.codex-tmp/spec189-b189-2-placement-build-r1/integration-identity.log).

This is a `FOCUSED_CXX_PASS` for the production request/ACK/Selection/handler
boundary. It is not T005 completion: the fixture does not expose a direct
counter proving that an unselected Provider performed zero layer fetches or
assembly before Selection, and it uses deterministic test runner material
rather than the real Qwen canonical manifest. The real MiniNDN runs, protected
candidate identity, selected-layer fetch counts and Provider assembly/output
oracle remain unobserved; B189-2 and T005 stay `PARTIAL`/`NOT_ACCEPTED`.

The first attempt used an incomplete Boost.Test filter and selected no test
cases; that command is retained in
[production-selection.log](../../../.codex-tmp/spec189-b189-2-placement-build-r1/production-selection.log)
as an invocation-boundary record and was not treated as a protocol result.
