# T015 evidence validator checkpoint (2026-08-23)

This checkpoint records the bounded evidence contract added for the model/task-first
streaming path. It is validation support, not a claim of a real provider or
MiniNDN workload run.

## Implemented

- `ndnsf-di-spec175-stream-evidence-v1` records lifecycle and timing metadata
  for Request, ACK closure, plan/Selection, preparation/prefill, decode epochs,
  internal feedback, external event delivery, End, Response, cancellation,
  retry, and backpressure.
- `validate_spec175_stream_evidence()` binds every record to one request,
  attempt epoch, and generation; requires contiguous sequence numbers and
  monotonic timestamps; and optionally enforces an exact event sequence.
- `Spec175StreamEvidenceRecorder` performs the same checks before accepting a
  span, bounds the retained record count, and writes only metadata JSONL on
  close.
- Role/provider attribution is mandatory for provider-owned and event transport
  spans. Recursive forbidden-field checks reject prompt/payload/answer/token,
  logits, KV, secret, and token-key material.
- Streamed Provider terminal publication uses the existing
  `TerminalAwareContext`, so `finish_stream()` releases the selection
  reservation before crossing the Core terminal guard.

## Focused validation

```text
42 passed
tests/python/test_streamed_invocation_api.py
tests/python/test_spec175_cpu_fixture.py
tests/python/test_spec175_evidence.py
tests/python/test_spec175_qwen_stateful_onnx.py
tests/python/test_spec175_streamed_generation.py
tests/python/test_spec175_qwen_generation.py
tests/python/test_spec175_contract_gate.py
```

Mutation coverage rejects missing, extra, reordered, or misattributed spans and
forbidden plaintext fields. This does not close T015: production TTFT/ITL span
emission and a real multi-provider provider/workload process remain open.
