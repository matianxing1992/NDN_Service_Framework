# R11-B8-G7 Native Cross-Process Revalidation

**Date**: 2026-09-10
**Status**: `CLOSED_FOR_VALIDATION` for this bounded process revalidation
**Parent**: R11-B8 / R11-B4 / R11-B5 / T010 / T016

## Scope

Freshly linked C++ `App_ServiceController`,
`DI_NativeArtifactAuthority`, `di-native-provider`, `DI_NativeRequester`,
and `DI_NativeOnnxAssemblyWorker` were exercised through the independent
NFD/Controller/Authority/Provider/requester fixture. Python only drove process
lifecycle and fixture setup; request, grant, Provider execution, stream
decoding, conversation state, and numerical oracles remained in C++.

## Validation

- YOLO/native unary process: requester returned `rc=0`, C++ numerical oracle
  emitted `NATIVE_NUMERICAL_ORACLE_PASS tensor=predictions values=4,0,12`,
  and Provider emitted verified grant plus real ONNX Runtime CPU execution
  evidence (`realCompute=true`, `cpuFallbackUsed=false`).
- Qwen/native stream conversation: first `FULL_CONTEXT` turn returned `rc=0`
  with eight-token C++ stream oracle and committed checkpoint; the same
  Provider completed second `APPEND_DELTA` turn with a fresh generation and
  returned `rc=0`; a wrong parent checkpoint returned `rc=1` with
  `DI_NATIVE_CONVERSATION_PARENT_MISMATCH`.
- Recovery control: after Provider restart, the second turn returned `rc=1`
  with `PROVIDER_CONVERSATION_STATE_MISSING`, and no duplicate execution was
  accepted. This is the intended bounded rejection until Provider KV recovery
  is implemented.

## Raw Evidence

- `.codex-tmp/spec182-r16-native-process-20260910/unary-driver.log`
- `.codex-tmp/spec182-r16-native-process-20260910/stream-conversation-success-driver.log`
- `.codex-tmp/spec182-r16-native-process-20260910/stream-conversation-recovery-short-driver.log`
- retained raw run roots under `/tmp/s182r16-unary-1789058849`,
  `/tmp/s182r16-conv-success-1789058824`, and `/tmp/s182r16-conv-1789058787`

This closes only the independent process revalidation boundary. Maintained
caller coverage, legacy zero-use, no-Python qualification, T015, T016, and
T017 remain open.
