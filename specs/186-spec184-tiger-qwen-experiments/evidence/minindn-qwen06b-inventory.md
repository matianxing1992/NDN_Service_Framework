# Spec186 Qwen3-0.6B MiniNDN inventory

**Date:** 2026-09-12
**Status:** `WAITING_EXTERNAL_INPUT`

The required Qwen3-0.6B weight and tokenizer were not found in the targeted
NDNSF roots. The only small GGUF discovered was:

```text
history checkout: third_party/qwen/qwen2.5-0.5b-instruct-q4_k_m.gguf
size: 491400032 bytes
sha256: 74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db
```

It is Qwen2.5 0.5B, not Qwen3-0.6B, and is rejected by the Spec186 profile
loader. No ONNX/Qwen3 stage manifest, Q3-compatible tokenizer or capacity
receipt can therefore be bound to `NDNSF_DI_Qwen06B_Native_Minindn.py`.

The waiting boundary is reproducible: provide a model and tokenizer, record
their SHA-256 and format (`onnx` or `gguf-q3`), verify the matching backend and
stage manifest, then run the native MiniNDN cold request followed by the
conversation follow-up under a new candidate/run identity. Existing Qwen3.6-27B
external qualification remains outside this row and is not closed here.
