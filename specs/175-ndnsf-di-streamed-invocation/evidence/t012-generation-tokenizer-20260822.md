# T012 generation, sampling, and tokenizer boundary

**Date**: 2026-08-22  
**Status**: complete

## Closed contract

- `SamplingConfig` seals Greedy and fixed-seed Top-K/Top-P behavior,
  repetition penalty, vocabulary bounds, and seed range.
- `GenerationTokenEventV1` binds request, token epoch, token ID, text delta, and
  accepted-prefix digest.
- `IncrementalDetokenizer` preserves committed token state, emits only the
  Unicode-safe suffix, accepts empty deltas, and fences pushes after EOS/stop.
- `StandaloneQwenTokenizer` uses the standalone `tokenizers` package and a
  digest-bound `tokenizer.json`; Qwen adapter modules contain no Transformers or
  PyTorch runtime import.

## Verification

```text
PYTHONPATH=NDNSF-DistributedInference \
  python3 -m pytest -q tests/python/test_spec175_qwen_generation.py
6 passed
```

The tiny CPU fixture remains the deterministic execution oracle. This task does
not qualify a 27B stateful CUDA artifact or a multi-Provider generation loop;
those remain T013-T015/T025-T027.
