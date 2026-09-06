# Spec175 Local Validation Contract

## Subject

One current source identity containing the generic streamed invocation, Qwen
prefill/decode adapter, Provider-owned state, and conversation continuation.
Historical candidates, SIFs, and Tiger results cannot be combined with this
subject.

## Ordered gates

```text
L0 design-code convergence PASS
  -> L1 complete native and Python suites
  -> L2 CPU/MiniNDN cold generation
  -> L3 CPU/MiniNDN continuation and mismatch rejection
  -> L4 local closure and Spec180 handoff
```

Focused failing tests and focused repair regressions may run inside L0. A
behavior-affecting change after L0 invalidates L0 and all later evidence.

## Required oracles

- One Request and ACK snapshot produce one committed plan and Selection.
- Every selected Provider owns exactly one complete role and executes it once
  per required epoch.
- Prefill occurs once; decode advances one token per epoch until the registered
  stop condition.
- Token events are complete, ordered, request/attempt/generation bound, and
  agree with the terminal Response.
- A same-conversation second turn binds the prior checkpoint and proves
  role-local state reuse; a mismatched checkpoint is rejected.
- Authentication, authorization, confidentiality, token, and replay checks are
  active on their production paths.
- All child exit statuses are collected and bounded cleanup leaves no owned
  process.

## Verdict

`LOCAL_FUNCTIONAL_PASS` requires every gate and oracle from one source identity.
Otherwise record `LOCAL_UNQUALIFIED` with the first failing layer. Neither
verdict makes a SIF, CUDA, Tiger, numerical real-model, or performance claim.

## Conformance markers

The following statements are normative contract markers consumed by the
repository gate; they restate behavior already owned by the contracts and are
not a second implementation authority:

- Qwen session roles use the canonical `/LLM/Pipeline/Stage/{index}` names.
- The registered workload uses `maxGeneratedTokens=64` and `maxEvents=65`.
- The deployed runtime has no runtime Transformers/PyTorch import.
- The Qwen profile is `modality=text-only`,
  `decodeMode=single-token-autoregressive`, `mtpEnabled=false`, and
  `thinkingMode=disabled`.
- The native canonical source binding uses `canonicalSourceDataName`,
  `canonicalSourceDigest`, and `canonicalSourceBytes`.
