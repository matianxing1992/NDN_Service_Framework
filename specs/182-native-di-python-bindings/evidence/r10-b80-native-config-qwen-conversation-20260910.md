# Spec182 R10-B80 Native Config Qwen Conversation and Tokenizer Authority

Date: 2026-09-10
Branch: Experimental
Decision: CLOSED_FOR_VALIDATION
Boundary: native runtime tokenizer authority and in-process real Core/Provider Qwen conversation

## Batch contract

| Lane | Covered boundary |
| --- | --- |
| Production entry/callers | nativeRequestRuntimeFromJson → NativeInferenceClient::request → Core BeginCollaboration → real ServiceProvider stream and conversation commit |
| Implementation/wire | NativeInferenceClient::commitConversationTurn now requires and persists runtime.contract.tokenizerDigest; model.semanticsDigest remains a separate graph/chat authority and cannot substitute |
| Test/harness/oracle | Spec182R10B80NativeConfigQwenRealProviderConversation checks the persisted transcript digest and terminal SPEC182_NATIVE_DI_REQUEST_RESULT_OK marker; the existing native-config stream selector remains in the sweep |
| Build/source closure | integration-tests rebuilt from the current source with system-first Waf -j2; changed-source Cppcheck and target registration were checked |
| Migration/evidence | Qwen source/catalog/runtime JSON remains native-owned; no Python planner or Python runtime participates in the test; independent executable worker transport, maintained caller migration and T016 remain open |

## Static review

The read-only review traced the commit path from NativeInferenceClient::commitConversationTurn
through NativeConversationCoordinator::prepareCheckpoint and the transcript validator. The old
fallback from an empty generation tokenizer digest to model.semanticsDigest was a contract
violation because those digests have different authorities. The repair validates that the runtime
contract and derived generation both carry the same tokenizer digest before opening the transaction,
then uses the runtime contract value for the persisted transcript. Cppcheck 1.90 exited 0; its
existing identical-condition/style diagnostics and integration-test parser errors were not
confirmed product defects.

## Validation

./waf build --targets=integration-tests -j2
-> PASS, 118/118, elapsed 2:45.86, maximum RSS 2,082,864 KB, swaps 0

integration-tests --run_test=Spec170NdnsfDiCoreFlow/Spec182R10B80NativeConfigQwenRealProviderConversation
-> PASS, 1 case, 11 assertions, exit 0, elapsed 6.94s

integration-tests --run_test=Spec170NdnsfDiCoreFlow/Spec182R10B73NativeConfigQwenRealProviderStream
-> PASS, 1 case, 5 assertions, exit 0, elapsed 3.63s

integration-tests --run_test='Spec170NdnsfDiCoreFlow/Spec182R10B*'
-> PASS, 5 cases, 26 assertions, exit 0, elapsed 21.08s

python3 -m pytest -q tests/python/test_spec182_native_closure.py
-> PASS, 41 tests

The new conversation selector observed the native business marker after the second result and
asserted that checkpoint.transcript.tokenizerDigest equals the operator-pinned runtime digest
and differs from model.semanticsDigest. This proves the authority boundary in the real
in-process Core/Provider conversation path; it does not prove independent DI_NativeRequester
and di-native-provider processes.

## Closure decision

CLOSED_FOR_VALIDATION applies to the tokenizer authority repair and native-config Qwen
in-process stream/conversation boundary. It does not close T010/T011/T013, worker/cross-process
transport, successful alternate-provider recovery qualification, maintained YOLO/Qwen caller
migration, legacy zero-use, I02–I08, T015, T016 or T017.
