# B7 C++ Current Candidate Qualification

## Status

`PARTIAL`。T013 的实现、逐任务静态门和批次组合门已通过，普通 compile-link 已恢复；聚焦 prepared-request C++ 运行已在有界 Face pump 修复后通过，但完整 process 矩阵、sanitizer、清理和最终批次收口仍未完成。

## Static and composition gates

- Frozen snapshot: `.codex-tmp/spec185-t013-review-v11-20260914`.
- Base: `c824a3fb`.
- Diff SHA256: `7a44c32d864dcb73bd7bad536202e07c6bbd42466185796df4335dfe26c37e80`.
- Paths SHA256: `b76e376e0ead4d982a1ddf00c50ffb2556117e18bee77fb21e6b6e061bf584f0`.
- Official `review-agent` SHA256: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`.
- Result: `STATIC_PASS`, then `B7_COMPOSITION_PASS`, both with no P0-P3 findings.
- Scope: `tests/integration-tests/di-prepared-process.t.cpp`, `tests/standalone/run-spec182-native-unary-process.py`, `tests/wscript`.

### Repair review v12

- Frozen snapshot: `.codex-tmp/spec185-t013-review-v12-20260914`.
- Base: `c824a3fb`.
- Diff SHA256: `e7490697a283386df5c49731360e8e91e6adf036f519018a2eefc4cab0f32712`.
- Paths SHA256: `7ebf1bbebff4e3fd77ef7a5bad58ce657c9adc21574388d4c877ada06e13fed2`.
- Result: official `review-agent` `STATIC_PASS`; no P0-P3 findings. The repair adds `OperationRuntime.cpp` exactly once to the manual `integration-tests` framework source closure and introduces no production behavior or duplicate target wiring.
- Review trace: five lanes covered—production/callers, state/lifecycle/error flow, C++ fixture/oracle, build/source closure, and migration/evidence. Compile-link, runtime, and sanitizer remain unobserved pending the next batch gate.

The composition gate verified that C++ owns the process selector and native oracle, Python only prepares NFD/PIB/TPM and process lifecycle, and the revoke case reads retained requester, Controller, and Provider logs directly from C++. The five B7 lanes and each native selector's two-run requirement are statically covered.

### Batch composition review v12

- Result: official `review-agent` `B7_COMPOSITION_PASS`; no P0-P3 findings.
- The combined flow keeps C++ ownership of the process selector, native assertions, revoke publication and Provider execution boundary; Python remains lifecycle orchestration. Unary/stream, conversation/recovery/replacement, deadline/cache/revoke, cleanup, bounded timeout, retained logs, and double-run requirements are wired in the C++ selector.
- Snapshot identity is the v12 static snapshot above. The composition review was read-only; compile-link v3 passed, while runtime, sanitizer, and residual-process cleanup remain pending.

### Runtime repair review v13

- Frozen snapshot: `.codex-tmp/spec185-t013-review-v13-20260914`.
- Base: `c824a3fb`.
- Diff SHA256: `ef3688ef12e744a9be5f4abe4eeb50c30e7a1a4778295f8dcdaf442fb1001c2c`.
- Paths SHA256: `6b717d23f180e720eab4a9d61c236614df9f23dd88b04387a8bb504252488e27`.
- Result: official `review-agent` `STATIC_PASS`; no P0-P3 findings. The stream catalog's `conversation_input` declaration matches the native adapter-owned encoder; revoke's 90-second bounded wait covers the 60-second Controller schedule while preserving baseline→revoke→post-revoke ordering. Python remains external lifecycle orchestration and C++ remains the oracle.
- Five lanes covered; the already linked v3 binaries are unchanged by this Python-only repair, so the next step is runtime re-test against the same candidate identity.

### Timeout fixture review v14

- Frozen snapshot: `.codex-tmp/spec185-t013-review-v14-20260914`.
- Base: `c824a3fb`.
- Diff SHA256: `263e8d14cb23debec2f383980cc39ccc1ebb57b7ff0d961dcef1466b2476f530`.
- Paths SHA256: `20201a5c1c58efed82f64557270108bb576b169f7564f82d97f91276772ab7ed`.
- Result: official `review-agent` `STATIC_PASS`; no P0-P3 findings. The concurrent externalized-selection fixture now uses bounded `timeout=10s` and `ackTimeout=5s` while preserving `ackTimeout < timeout`; conversation encoder declaration, 90-second revoke wait, baseline ordering, C++ oracle, and Waf closure remain intact.
- Five lanes covered. Because the C++ fixture changed, a fresh batch compile-link is required before runtime retry.

### Bounded pump review v15

- Frozen snapshot: `.codex-tmp/spec185-t013-review-v15-20260914`.
- Base: `c824a3fb`.
- Diff SHA256: `37db1c903cf8299b5a53404816b754bc39934fe0a1c697d19819ad3ab9be3bf4`.
- Paths SHA256: `20201a5c1c58efed82f64557270108bb576b169f7564f82d97f91276772ab7ed`.
- Result: official `review-agent` `STATIC_PASS`; no P0-P3 findings. The focused C++ fixture now repeats its bounded `pumpUntil` round up to four times, allowing the 5-second ACK deadline to be driven by the real User Face scheduler before future waits; production timeout and ownership semantics are unchanged.
- Five lanes covered. The v15 static gate is complete; compile-link, runtime qualification, and sanitizer remain pending.

### Repair review v17

- Frozen snapshot: `.codex-tmp/spec185-t013-review-v17-20260914`.
- Base: `c824a3fb`.
- Diff SHA256: `32065d2bdcb3e0da550d9953ddb532108d9c3da691e30fe848edc69202edfa70`.
- Paths SHA256: `56b65200d3bb201f672f0efd7ac07e381f9098cf7bf44e427b715dd3a2d4d51f`.
- Result: official `review-agent` `STATIC_PASS`; no P0-P3 findings. The production catalog now supports `TENSOR_BUNDLE_TOKEN_IDS`, extracting the configured Int64 token tensor from the authenticated request bundle; the stream fixture selects `input_ids`, matching `NativeEpochCoordinator`'s execution lineage. The ordinary stream branch indentation and `[3]` canonical fixture input are restored. Five lanes are statically covered; compile-link, runtime, sanitizer, cleanup, and final composition remain unobserved.

## Compile-link boundary

Command boundary: system-first `/usr/bin/g++ -B/usr/bin`, `CXX=/usr/bin/g++`, existing `build-spec185-b0c-normal`, `-j4`, targets `spec185-process`, `spec185-prepared-request`, `spec185-provider-assembly`, and `integration-tests`.

- Raw log v1: `.codex-tmp/spec185-b7/process-build-v1.log`; return code `.rc` = `1`; resource trace `.vmstat.log`.
- v1 first failure: `tests/integration-tests/di-prepared-process.t.cpp` referenced `requireAbsentMarker` after the helper had been removed during the C++ revoke-oracle refactor. The build stopped during translation; no target linked and no selector ran.
- Repair: helper restored in the same C++ test scope; v11 immutable static-review snapshot and composition review both passed.
- Raw log v2: `.codex-tmp/spec185-b7/process-build-v2.log`; return code `.rc` = `1`; resource trace `.vmstat.log`.
- v2 boundary: `spec185-process` linked successfully, then the existing `integration-tests` target failed at link with undefined `ndn_service_framework::OperationRuntime` methods (`notifyWaiters`, `drain`, `drainAsync`, `create`, `close`) referenced by DI `Runtime.cpp`. No selector ran. The manual `framework_sources` closure in `tests/wscript` omitted `ndn-service-framework/OperationRuntime.cpp`; this is a build-registration repair boundary, not a runtime result.
- Repair review: after adding the missing source, v12 static review passed with the immutable snapshot and hashes above; the batch is eligible for a fresh compile-link attempt.
- Compile-link v3: system-first `/usr/bin/g++ -B/usr/bin`, `CXX=/usr/bin/g++`, existing `build-spec185-b0c-normal`, `-j4`, targets `spec185-process`, `spec185-prepared-request`, `spec185-provider-assembly`, and `integration-tests` returned `rc=0` in 18.80s. Raw log `.codex-tmp/spec185-b7/process-build-v3.log`; return code `.rc` = `0`; resource trace `.vmstat.log` showed no sustained swap. All four requested binaries linked; runtime-test and sanitizer remain pending.
- Compile-link v4: after the C++ fixture budget repair, the same system-first `-j4` target set returned `rc=0` in 28.79s; `spec185-prepared-request` was recompiled and linked, and the other three targets remained valid in the same build tree. Raw log `.codex-tmp/spec185-b7/process-build-v4.log`; return code `.rc` = `0`; resource trace `.vmstat.log` showed no sustained swap. Candidate runtime tests may now reuse this build.
- Compile-link v5: after the bounded pump repair, the same system-first `-j4` target set returned `rc=0` in 107.22s. The current worktree timestamp caused `ServiceUser.cpp` and the focused fixture to recompile; all four targets linked in the existing build tree and the resource trace showed no sustained swap. Raw log `.codex-tmp/spec185-b7/process-build-v5.log`; return code `.rc` = `0`; runtime and sanitizer evidence remain separate.
- Compile-link v6: after the v17 production encoder and stream fixture repair, the same system-first `-j4` target set returned `rc=0` in 28.79s. `NativeRequestCatalog.cpp` recompiled in normal, test, provider-test, and integration variants; all four targets linked in the existing build tree and the resource trace showed no sustained swap. Raw log `.codex-tmp/spec185-b7/process-build-v6.log`; return code `.rc` = `0`; runtime and sanitizer evidence remain separate.

## Runtime boundary v1

- Raw process selector: `.codex-tmp/spec185-b7/process-runtime-v1.log`; return code `.rc` = `201`.
- Retained failed runs: `.codex-tmp/spec185-b7/runtime-v1/stream-conversation/`, `.codex-tmp/spec185-b7/runtime-v1/unary-revoke/`, and `.codex-tmp/spec185-b7/runtime-v1/spec185-b7-selector-spec185-prepared-request-2128899-1.log`.
- `ConversationRecoveryAndReplacementRemainTerminal` stopped at the first conversation case: native requester returned `NATIVE_REQUEST_STAGE_FAILED code=UNSUPPORTED_CAPABILITY boundary=input` because the stream catalog did not declare its operator-pinned `conversation_input` encoder. This is a fixture contract omission; replacement/recovery cases were not qualified by this run.
- `NativeNegativeCacheDeadlineRevokeAndCleanupMatrix` stopped in `PreparedRequestCompletesThroughProvider` with `DiError: native result wait timed out` after the selector reached `NDNSF_INTEGRATION_BOOTSTRAP_READY`; no selector PASS is claimed.
- `RealCrossProcessRevokeFailsClosed` stopped in the Python lifecycle orchestrator waiting only 30 seconds for `NDNSF_REVOCATION_APPLIED success=1` while Controller revocation was scheduled for 60 seconds. The Controller marker was not observed; no revoke qualification is claimed.
- These are separate runtime boundaries. The next fixes are limited to the stream fixture's conversation declaration, the revoke wait budget, and focused diagnosis of the prepared-request timeout; each code change requires a new immutable snapshot and static review before retry.
- Focused timeout reproduction: `.codex-tmp/spec185-b7/prepared-timeout-focus-v1.log` with return code `.rc` = `201` runs only `Spec185PreparedRequest/PreparedRequestCompletesThroughProvider`. It reaches `NDNSF_INTEGRATION_BOOTSTRAP_READY` and deterministically returns `DiError: native request budget expired` at about 5.8s; no result or cleanup assertion is reached. This diagnostic run changes no source and is not a qualification result.
- Trace diagnosis: `.codex-tmp/spec185-b7/prepared-timeout-trace-v1.log` reaches Provider `RESPONSE_PUBLISHED` for request `...-1` at about 4.74s and accepts that response, but the second request's ACK callback is observed after its 2-second ACK deadline (`ACK_SKIPPED_AFTER_ACK_WINDOW`). The fixture's 5-second request/2-second ACK budgets are therefore too tight for two concurrent externalized selections; no production code failure is inferred. The focused trace is retained as a diagnostic boundary.
- Focused retry v2: `.codex-tmp/spec185-b7/prepared-timeout-focus-v2.log` with `.rc` = `201` still returns `native request budget expired` after the fixture budget was widened to 10s/5s (last checkpoint line 883). The retry did not qualify the case; a new trace is required before further changes.
- Focused trace v2: `.codex-tmp/spec185-b7/prepared-timeout-trace-v2.log` with `.rc` = `201` shows both requests' ACKs matched in about 100ms (`ackWindowExpired=false`), but neither request emitted `COLLAB_ACK_CLOSED`, selection, or Provider execution before the 10-second request budget expired. A temporary diagnostic confirmed `scheduleAckTimeout=true`, `scheduleImmediateAckTimeout=true`, and `ackTimeoutMs=5000` for both requests; no `ACK_TIMEOUT_CALLBACK` was observed. `strace` also showed no timerfd armed for the 5-second ACK deadline. This is a production scheduler/state-machine boundary, not a fixture budget qualification; the diagnostic logging is temporary and is not itself a PASS.
- Focused retry v3: `.codex-tmp/spec185-b7/prepared-timeout-focus-v3.log` with `.rc` = `0` runs only `Spec185PreparedRequest/PreparedRequestCompletesThroughProvider` against the v5 candidate. The case completed in approximately 11.09s with `*** No errors detected`; the four-round bounded `pumpUntil` continued driving the User Face scheduler through the 5-second ACK boundary before waiting on futures. This closes the prior focused test-driver boundary, but it is not the full process qualification.

## Coverage status

| Lane | Status | Boundary |
| --- | --- | --- |
| production/callers | STATIC_PASS | Native process selector invokes public C++ target and existing native processes |
| lifecycle/state/concurrency | `PARTIAL` | static bounded process groups passed; focused prepared-request path now passes, while conversation/revoke/full matrix remain pending |
| fixture/oracle | `PARTIAL` | focused C++ log/boundary oracle passes; complete process matrix and cleanup assertions remain pending |
| build/source closure | `PASS` | v5 linked `spec185-process`, B3/B5 selectors, and `integration-tests` after the closure repair |
| migration/evidence | `PARTIAL` | v5 candidate and focused runtime identity are retained; full qualification and sanitizer evidence remain pending |

No runtime PASS, no ASan/UBSan/LSan result, and no final B7 closure is claimed. T013 remains `PARTIAL` pending repair and fresh static review, compile-link, and runtime evidence.

### Conversation runtime v3

- `.codex-tmp/spec185-b7/process-runtime-conversation-v3.log` with `.rc` = `0` runs only `Spec185Process/ConversationRecoveryAndReplacementRemainTerminal` against the v6 candidate. The selector completed in approximately 116.35s with `*** No errors detected`; both conversation checkpoint/second-turn runs, recovery state-missing failures, replacement attempt-2 runs, and no-backup fail-closed runs passed their C++ log oracles twice. This closes the conversation/recovery/replacement submatrix, but revoke, the remaining process matrix, sanitizer, cleanup, and final composition remain pending.

### Full process runtime v3 boundary

- `.codex-tmp/spec185-b7/process-runtime-v3.log` with `.rc` = `201` completed unary/stream, conversation/recovery/replacement, and the negative cache/deadline/revoke prelude. `RealCrossProcessRevokeFailsClosed` reached baseline requester success, Controller revocation, and revoked requester fail-closed, then failed only because the C++ oracle required optional `NDNSF_DI_PROVIDER_HANDLER_TIMING event=start`; the retained Provider baseline contains the production `event=PROVIDER_EXECUTE_DONE` marker. The run is not a full runtime PASS; raw run `/tmp/spec185-b7-unary-2204547-13/` is retained.

### Repair review v18

- Frozen snapshot: `.codex-tmp/spec185-t013-review-v18-20260914`.
- Base: `c824a3fb`.
- Diff SHA256: `0549e53000e79aadd8665801cbfde66058b39051e9d0768d22ac046c1ee4f1db`.
- Paths SHA256: `b227804f0d93ccae7b4548db087215b801b1f5b25797fdfeacb47f3ff16a2296`.
- Result: official `review-agent` `STATIC_PASS`; no P0-P3 findings. The revoke oracle now counts the production `event=PROVIDER_EXECUTE_DONE` trace from `ServiceProvider::finishRequestExecutionOnEventLoop`, keeping baseline/post-revocation log boundaries and the zero-execution-after-revocation assertion. Compile-link, runtime, sanitizer, cleanup, and final composition remain pending.

### Final composition review v19

- Frozen snapshot: `.codex-tmp/spec185-t013-composition-v19-20260914`.
- Base: `c824a3fb`.
- Diff SHA256: `e3fd98d94cd22d8637cca8d9c62de4c5ee02c60073d59b0775460f84117fac8b`.
- Paths SHA256: `56b65200d3bb201f672f0efd7ac07e381f9098cf7bf44e427b715dd3a2d4d51f`.
- Result: official `review-agent` `B7_COMPOSITION_PASS`; no P0-P3 findings. Production/callers, state/lifecycle/concurrency, C++ fixture/oracle, build/source/migration, and validation-gap lanes are covered; the dynamic qualification remains `OPEN_FOR_VALIDATION` until runtime and sanitizer evidence are recorded.

### Compile-link v7

- After v18 static review, system-first `/usr/bin/g++ -B/usr/bin`, `CXX=/usr/bin/g++`, existing `build-spec185-b0c-normal`, `-j4` rebuilt only the affected `spec185-process` target. `di-prepared-process.t.cpp` recompiled and the selector linked with `rc=0` in 9.00s; the other v6 targets remain the same production candidate. Raw log `.codex-tmp/spec185-b7/process-build-v7.log`; return code `.rc` = `0`; resource trace `.vmstat.log` showed no sustained swap. Runtime, sanitizer, cleanup, and final composition remain pending.

### Revoke runtime v4

- `.codex-tmp/spec185-b7/process-runtime-revoke-v4.log` with `.rc` = `0` runs only `Spec185Process/RealCrossProcessRevokeFailsClosed` against the v7 selector twice, completing in approximately 133.28s with `*** No errors detected`. Both retained runs (`/tmp/spec185-b7-unary-2216576-1/` and `-2/`) prove baseline requester success, `NDNSF_REVOCATION_APPLIED success=1`, revoked requester `DI_NATIVE_NO_ADMITTED_PROVIDER` and failure terminal, plus a baseline Provider `event=PROVIDER_EXECUTE_DONE` and zero such events after the retained log boundary. This closes the revoke submatrix; sanitizer, cleanup, and final B7 closure remain pending.

### Repair review v22

- Frozen snapshot: `.codex-tmp/spec185-t013-review-v22-20260914`.
- Base: `c824a3fb`.
- Diff SHA256: `c096cdcf2b763d9407863673bcbc419c821cfd5d71f769f50f27ebbd9551a101`.
- Paths SHA256: `4a8f0f88415fa22f9cf6f5f27c43c4da9888b8e637acc73f2f0b007e2e224d09`.
- Result: official `review-agent` `STATIC_PASS`; no P0-P3 findings. The C++ fixture now separates result state from the thread owner, makes cancellation/join cleanup non-throwing, cancels active request handles on initialization failure, and removes `std::async`/future predicates from positive, negative, and drain paths. Compile-link and runtime remained unobserved at this review boundary.

### Final composition review v23

- Frozen snapshot: `.codex-tmp/spec185-t013-composition-v23-20260914`.
- Base: `c824a3fb`.
- Diff SHA256: `f113752f96814e36081dd021ffa623f24bce6b82c516ed08b5f17ef741c8a690`.
- Paths SHA256: `56b65200d3bb201f672f0efd7ac07e381f9098cf7bf44e427b715dd3a2d4d51f`.
- Result: official `review-agent` `B7_COMPOSITION_PASS`; no P0-P3 findings. Production/callers, state/lifecycle/concurrency, C++ fixture/oracle, build/source closure, migration and evidence boundaries are covered. Dynamic qualification remained `OPEN_FOR_VALIDATION` until the following candidate checks.

### Compile-link v8

- After v22 static and v23 composition review, system-first `/usr/bin/g++ -B/usr/bin`, `CXX=/usr/bin/g++`, existing `build-spec185-b0c-normal`, `-j4` rebuilt the affected `spec185-prepared-request` target. The fixture translation and link completed with `rc=0` in 24.92s; raw log `.codex-tmp/spec185-b7/process-build-v8.log`, return code `.rc` = `0`, resource trace `.vmstat.log` showed no sustained swap.

### Focused normal runtime v4

- `.codex-tmp/spec185-b7/prepared-request-runtime-v4.log` and `.rc` = `0` run `Spec185PreparedRequest/PreparedRequestCompletesThroughProvider` against the v8 candidate. The C++ selector completed in approximately 17.87s with `*** No errors detected`, including positive provider execution, wrong-digest rejection, revocation fail-closed and runtime drain through the explicit observation/join fixture.
- `.codex-tmp/spec185-b7/prepared-conversation-runtime-v4.log` and `.rc` = `0` run `Spec185PreparedRequest/PreparedConversationCommitsTwoNativeTurns` against the same v8 candidate. The C++ selector completed in approximately 15.52s with `*** No errors detected`, including two native turns, checkpoint export/recovery, replacement and drain cleanup.

## Historical B7 status after v23/v8

The repair and normal focused selectors pass with the new C++ fixture owner model. T013 remains `PARTIAL`: the full process matrix is covered by the previously recorded normal process/revoke evidence, but the changed prepared-request candidate still requires the sanitizer rebuild and runtime/cleanup boundary. No sanitizer or final T013 qualification PASS is claimed here.

## B7 served-provider integration and qualification 2026-09-15

### Scope and static gate

The served-provider test adds a C++ integration path from the public `Runtime::open`/`user().prepare` API through native planning, assignment fetch, ACK admission, authenticated grant verification, `Provider::serve`, runner execution, and the final response. The test intentionally keeps the preparation, runner, protected-runtime, ACK and grant publication seams local to the C++ fixture; the production provider assembly and authority-wire paths remain covered by the existing `spec185-provider-assembly` selectors.

The immutable v63 snapshot `.codex-tmp/spec185-t013-served-provider-review-v63-20260915` was reviewed by the official `/home/tianxing/.codex/skills/review-agent/SKILL.md` and returned `STATIC_PASS`; batch composition returned `B7_COMPOSITION_PASS`, both with no P0-P3 findings. Snapshot identity: base `6b523b3c8d511c325d8084a8c62959b184a95270`, changes SHA256 `7c2653842a1eb44fb07563b569703d92f4e28dd53148fe5d105c1a8fb22069ee`, paths SHA256 `a9cafb1a364d6018a89f0b4dca1ead1285e8ffc150de2a341e9263e8a1cfb6d1`, source SHA256 `e9119d85f5e7d1e7f369a41b570d074a8e689ffcf2845ac273deee6c6a4012de`.

The final v63 composition review reconfirmed the same immutable identity: `PreparedModel → Runtime → Core ACK/Selection/grant → Provider::serve → NativeProviderHandler → backend runner → Response`; test-only ACK injection remains behind `NDNSF_DI_PROVIDER_TEST_SEAM`, and the C++ selector owns the business oracle. The review retained P2 coverage limits only: the served case does not independently qualify production canonical assembly, remote authority publication/fetch, or multi-provider/fallback/wrong-epoch/wrong-grant behavior; those remain covered by the existing provider-assembly selectors and prior evidence.

### Defects exposed by the integration path

These failures were retained before repair; none is counted as a passing result.

1. `prepared-served-build-v1.rc=1` stopped at translation because the new test used `nativeProtectedFencingToken` without including `NativeProtectedProvider.hpp`. The include was added and the affected targets rebuilt.
2. `prepared-served-focused-v1.rc=201` reached bootstrap and then SIGSEGVed in `DummyClientFace::receive`/`Face::satisfyPendingInterests`. The fixture was delivering bridge packets while the borrowed Face worker was being scheduled; `deferBridgeDelivery=true` was added so bridge delivery is driven after the worker is ready.
3. `prepared-served-focused-v2.rc=201` reached ACK and grant verification but correctly rejected `published source is not authorized`. The fixture's `NativeGrantPublicationSource` was completed with the model, canonical-source, content, initializer and artifact-profile digests used by the authorized manifest.
4. `prepared-served-focused-v3.rc=201` reached assignment, ACK and grant verification but failed with `no NativeModelRunner backend registered: onnxruntime-cpu`. The served fixture now registers the exact `onnxruntime-cpu` backend selected by the offer and `NativeModelRunnerSpec`.
5. Normal matrix v35 and sanitizer matrix v2 each hit the existing `RuntimeDrainAsyncIncludesNativeClientWork` 60-second notification boundary once. The same selector passed isolated twice (`runtime-drain-native-client-asan-focused-1/2.rc=0`), and the subsequent complete normal and sanitizer matrices passed; no source change was made for this transient boundary.

### Compile-link and runtime results

- Normal affected-target build v5 (`.codex-tmp/spec185-b7/prepared-served-build-v5.log`, `.rc=0`) linked `spec185-prepared-request`, `spec185-prepared-conversation`, `spec185-provider-assembly` and `spec185-process` after the backend repair.
- The independent ASan/UBSan build v2 (`.codex-tmp/spec185-b7/asan-affected-build-v2.log`, `.rc=0`) used `-j2` because the 6-core/12-GB host reached approximately `3.52 GB` peak RSS under sanitizer instrumentation; elapsed time was `25:29`. This is a resource decision, not a product result.
- Normal served-provider focused C++ selector v4 (`.codex-tmp/spec185-b7/prepared-served-focused-v4.log`, `.rc=0`) completed in `8.76s`, emitted `NDNSF_DI_GRANT_VERIFICATION` with `boundary=BEFORE_ASSEMBLY`, and emitted `NDNSF_COLLAB_FINAL_RESPONSE event=publish_requested`; Boost.Test reported `*** No errors detected`.
- ASan/UBSan served-provider focused selector (`.codex-tmp/spec185-b7/prepared-served-asan-focused-v1.log`, `.rc=0`) completed in `9.15s`, emitted the verified grant marker, and produced no sanitizer report.
- The unfiltered normal `spec185-process` selector (`.codex-tmp/spec185-b7/process-runtime-normal-full-v1.log`, `.rc=0`) ran all five process test cases in `6:50.61` with `*** No errors detected`. It covered unary/stream, conversation/recovery/replacement, C++ deadline/cache/revoke/cleanup selectors, cross-process revoke, and the served-provider case through the process matrix.
- The sanitizer C++ matrix (`.codex-tmp/spec185-b7/process-runtime-asan-v4.log`, `.rc=0`) completed in `2:42.34`, peak RSS `984712KB`, with `*** No errors detected`. The matrix launched 38 isolated native selectors (each case twice); all 38 selector logs contained the Boost success marker and none contained `AddressSanitizer`, `UndefinedBehaviorSanitizer`, or `runtime error:`.

### Coverage and remaining boundary

The current C++ integration evidence now observes the full public request-to-response chain and the negative/lifecycle paths in the B7 matrix. It can catch build-registration, wiring, source-authorization, backend identity, protocol-state and lifecycle failures; the retained logs show that it did catch each of the first four classes during this run. The served-provider happy path still uses explicit test seams for preparation, runner, protected runtime and local publication, so it does not by itself qualify the production canonical assembler or a remote authority publication. Those production paths remain represented by the existing provider-assembly selectors.

T013 therefore remains `PARTIAL` until the final source/ELF/no-Python identity convergence and batch closure record are refreshed against this candidate. No Python-only assertion is used to claim native behavior, and no SIF/Tiger or large-model qualification is implied.
