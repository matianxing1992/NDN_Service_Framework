# Spec175 production streamed-generation wiring audit

**Date**: 2026-08-23  
**Verdict**: source wiring `PASS`; T014/T015 and complete G2 remain `BLOCKED`
until the registered production-process evidence exists

## Verified seams

- The production C++ `NativeEpochCoordinator` can execute one unary ONNX
  transition per selected role and epoch, carry complete state locally, move
  activation and TOKEN_FEEDBACK over authenticated DATA_V1, emit token events
  only from the terminal role, and terminate every selected Provider.
- Native I01-I03/I15 pass through real Core Request/ACK/plan/Selection and the
  checked-in tiny stateful ONNX fixture when the integration harness explicitly
  supplies the generation plan and Provider config.
- The Python `AutomaticPlanningCoordinator.request_streaming(...)` creates one
  logical handle and labels the request `TOKEN_STREAMING`.

## Source-wiring closure

The former production gap is closed in the current source:

1. `AutomaticPlanningCoordinator` adds the exact terminal-to-first-role
   `TOKEN_FEEDBACK` dependency, derives a nonzero
   `streaming_operation_stride`, and seals the bounded per-epoch operations in
   the Provider capabilities.
2. `GenerationExecutionContractV1` seals the token input, maximum generated
   tokens, EOS IDs, sampling mode/digest, operation stride, and complete state
   input/output names into the V3 plan and Provider projection.
3. Qwen `SplitCandidate` now carries adapter-owned per-role state I/O for
   `attention_kv`, `recurrent_state`, and `convolution_state`; V3 role assembly
   copies those contracts instead of reconstructing them in a test fixture.
4. `NativeExecutionPlanJson` parses and validates the generation contract.
   `NativeProviderHandler::generationConfigFromAuthenticatedRequest(...)`
   derives the coordinator configuration from the authenticated request and
   projection, and the handler invokes `runNativeEpochCoordinator(...)`.
5. `DI_NativeProviderExecutable` rejects a capability whose plan digest does
   not equal the authenticated `executionPlanDigest`.
6. The production Python user requests `useCache=true` and
   `outputMode=TOKEN_STREAMING`; the legacy Python Provider fails closed when
   that mode lacks the stateful cache contract.

The contract gate now finds all 58 functional requirements and 12 success
criteria and reports only `DIRTY_INPUT_TREE`. The focused production-wiring
Python suite passes 60/60. A complete `/usr/bin/g++`/Boost-1.71 build succeeds,
and the focused native suites pass 57/57 cases (`NativeV3`,
`DiQwenGenerationSession`, `Spec175InvocationStreamLifecycle`, and
`Spec175InvocationStreamMessage`). The rebuilt integration binary passes all
14 registered Spec175 native tiny-ONNX CPU cases and all 15
`Spec175InvocationStream` cases.

One integration assertion initially expected both selected roles to receive a
user-facing stream publisher. Runtime instrumentation showed the contract was
working as designed: all roles receive the authenticated request options, but
only the selected terminal role receives the event-key grant and publisher.
The stale assertion was corrected to verify that ownership boundary; no
runtime bypass was added.

## Remaining production evidence gap

The source closure does not by itself complete T014 or T015. The registered
I01-I03/I15 harness proves the same native coordinator with tiny stateful ONNX,
but the project still lacks the frozen P1/P2 `workload.json` and one fresh
process run from the real Python model/task-first user through automatic V3
planning into native Providers. Formal I12/I13 retry cases and the complete
three-process G2 matrix also remain absent. Real Qwen3.6-27B stateful CUDA
qualification belongs to T013/G5/G6 and is not implied by the tiny CPU result.

## Required closure

- Freeze and validate the exact P1/P2 workload before G3.
- Run the real Python workload/User against native Providers in fresh
  processes and prove one Request/plan/prefill plus per-token state continuity.
- Add formal real-transport I12/I13 and execute the entire I01-I15 G2 matrix
  three times without skips.
- Keep T013 and Qwen3.6 CUDA qualification separate from the tiny CPU contract
  proof.
