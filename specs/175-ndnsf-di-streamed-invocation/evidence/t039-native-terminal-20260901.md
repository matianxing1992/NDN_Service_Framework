# T039 Native terminal-generation closure — 2026-09-01

## Acceptance

The rebuilt production integration binary is exercised without importing the
host native Python extension.  The process oracle covers:

- I01 one-Provider Greedy EOS generation;
- I03 four-Provider Greedy generation and coordinator completion counts; and
- I16 four-Provider seeded Top-K/Top-P generation with the UTF-8 tokenizer,
  split stop sequence, ordered Unicode deltas, final text, token IDs, and stop
  reason.

The production-path source gate also rejects two previously under-specified
mutations: removing the authenticated sampling mode so only a sampling digest
remains, and replacing the decoded suffix with an always-empty `textDelta`.

## Focused evidence

```text
python3 -m pytest -q \
  tests/python/test_spec175_contract_gate.py \
  tests/python/test_spec175_native_oracle.py \
  tests/python/test_spec175_native_assembly_helper.py \
  tests/python/test_spec175_native_assembly.py
  23 passed
```

`test_spec175_native_oracle.py` verifies the exact token transcripts, real
per-event text deltas, concatenated terminal text, EOS/stop finish reason, and
four-Provider completion/failure counts.  The mutation test verifies that a
digest-only sampling path returns `NATIVE_SAMPLING_AUTHORITY_MISSING` and an
always-empty delta returns `NATIVE_TEXT_DELTA_CONTRACT_MISSING` before any
broad run can be accepted.

## Boundary

This closes the native terminal owner at the implementation boundary. Python
3.10/SIF replay remains T023 evidence, and the final performance/correctness
qualification remains owned by T020/T022/T025--T028.
