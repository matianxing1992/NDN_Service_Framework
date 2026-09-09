# R10-B41 Native Stream Business-Oracle Closure

日期：2026-09-09
状态：`DONE` for the bounded stream/unary business-oracle boundary; T016 remains `PARTIAL`.

## Scope and allocation

- **Production entry/callers**：`NativeInferenceClient::request` → `ServiceUser::BeginCollaboration` → the R4-B6 real `ServiceProvider` callback, then the canonical MiniNDN owner/runner.
- **Implementation/wire**：`runR4B6RealProviderConversationCase` now emits `SPEC182_NATIVE_DI_REQUEST_RESULT_OK` only after the unary or stream-only native result assertions succeed. The existing conversation marker remains after the second turn.
- **Test/harness/oracle**：`Spec182R10B37RealProviderNativeStreamRequest`, `Spec182R10B31RealProviderUnaryRequest`, `Spec182R10B33RealProviderUnaryRepositoryReferenceRequest`, R4-B6 conversation/replacement/alternate-replacement/repository selectors, and the PO-001 `businessOracle.stdoutMarker`.
- **Build/source closure**：the integration target was rebuilt with system-first `/usr/bin/g++ -B/usr/bin` and `-j2` after the prior nonzero swap-in observation. The actual fresh linked output was `.codex-tmp/spec182-r4-b2/build/integration-tests`; the older `build-nac182/integration-tests` path was not reused for the isolated manifest.
- **Migration/evidence**：this closes the stream/unary marker boundary and one isolated PO-001 native-process owner/runner case. It does not close independent requester/Provider transport, maintained caller migration, no-Python qualification, or the remaining T016 matrix.

## Static review

Read-only `review-agent` review covered the complete source diff, both helper branches, result assertions, marker placement, selector registration, integration target registration, runner business-oracle consumption, artifact source/digest binding, and the owner/runner result shape. No actionable finding remained. The review also retained the stale-build-path miss as a changed runtime gate rather than treating the first isolated attempt as a product failure.

## Verification

- `git diff --check` passed for the source change.
- Fresh integration build: `PATH=/usr/bin:/bin:/usr/sbin:/sbin /usr/bin/python3 ./waf -o build-nac182 build --targets=integration-tests -j2 -v`; Waf `118/118`, exit `0`, about `40.562 s`; generated target identity is recorded in `.codex-tmp/spec182-r10-b41/build.log`.
- Direct current-binary selectors all exited `0`: stream-only (4 assertions), unary, unary `REPO_REF`, conversation, replacement negative, alternate replacement, and conversation `REPO_REF`. The stream and unary selectors printed `SPEC182_NATIVE_DI_REQUEST_RESULT_OK` only after their native result checks.
- Fresh root owner/runner `r16`: corrected runner manifest includes `process.role=requester`, points at the current `.codex-tmp/spec182-r4-b2/build/integration-tests`, and uses its recomputed SHA-256. PO-001 evaluation is `PASS`; native process return code `0`; `timedOut=false`; business marker, identity, namespace, process-tree, endpoints, trace, and cleanup evidence are all present; owner exit `0`.

## First boundaries and changed gate

Earlier raw runs remain immutable. `r13`/`r14` exposed selector/build and missing-marker boundaries. `r15` used the repaired source but staged the older `build-nac182/integration-tests`, so the process exited `0` with complete structural observation but `MISSING_EVIDENCE:business-oracle`; this was an artifact-source identity miss. The changed gate for `r16` was to bind the runner manifest to the actual fresh linked output and recompute its digest. No protocol failure was observed.

## Closure decision

`CLOSED_FOR_VALIDATION` for the native stream/unary business-oracle and one isolated PO-001 owner/runner case. `OPEN_FOR_NEXT_BATCH` for I01–I08, PO-002–PO-014, independent Provider process transport, maintained YOLO/Qwen callers, no-Python migration, and complete T016 qualification.

Raw runs: `.codex-tmp/spec182-r10-b41/`, `.codex-tmp/spec182-t016-r13/`, `.codex-tmp/spec182-t016-r14/`, `.codex-tmp/spec182-t016-r15/`, `.codex-tmp/spec182-t016-r16/`.
