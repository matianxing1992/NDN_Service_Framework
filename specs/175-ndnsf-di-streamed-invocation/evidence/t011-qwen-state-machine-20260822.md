# T011 Qwen generation state machine

**Date**: 2026-08-22  
**Status**: complete

## Closed contract

- Qwen session roles are the canonical `/LLM/Pipeline/Stage/0..2` names.
- The signed output bound is 64 tokens; the state machine rejects 65 and
  cannot advance beyond the bound.
- EOS and stop-sequence completion are legal before the maximum-token count,
  while `MaxTokens` requires the exact count.
- Completion now requires the same finish reason recorded by `beginDrain()`;
  there is no remaining no-argument `complete()` caller.
- Deadline, cancellation, stale attempt, one-replacement, duplicate terminal,
  and terminal-response claim rules remain fenced and bounded.

## Verification

```text
./waf build --target=unit-tests -j2                  PASS
build/unit-tests --run_test=DiQwenGenerationSession  14 cases PASS
```

This closes the state-machine contract only. It does not claim that a Qwen
ONNX graph, tokenizer, or multi-Provider streamed generation loop is wired;
those remain T012-T015.
