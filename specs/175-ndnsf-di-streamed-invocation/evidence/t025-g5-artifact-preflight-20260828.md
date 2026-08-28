# T025 G5 artifact preflight — 2026-08-28

## Result

G5 was not started.  The only Qwen3.6-27B ONNX bundle currently staged on
Tiger is an older fixed-context export and fails the repository's stateful
manifest contract before any model execution or allocation.  It must not be
reported as stateful readiness, CUDA cache effectiveness, G6, G6C, or G7
evidence.

## Subject inspected

| Field | Value |
|---|---|
| Remote bundle | `/project/tma1/ndnsf-di/evidence/spec175/qwen36-onnx/spec175-qwen36-onnx-202864` |
| Manifest | `qwen-onnx-service-manifest.json` |
| Manifest SHA-256 | `sha256:2fe6af0e1c65e3f7a5394e619e7f7f28b7ba89ff4d85b4092c264644ae650433` |
| ONNX archive | `onnx-stage-artifacts.tar` |
| ONNX archive SHA-256 | `sha256:bdc4dd06cac7240de2e8b2dce9f7e483fb4416a54398a11c926d47ad1170c093` (the 25 GB archive was not recopied locally) |
| Model | `Qwen/Qwen3.6-27B@6a9e13bd6fc8f0983b9b99948120bc37f49c13e9` |
| Runtime | ONNX Runtime, `CUDAExecutionProvider` |
| Context contract | `fixed-context-padded-v1`, context length 96 |

The remote manifest has no `decodeMode`, `modality`, `mtpEnabled`,
`thinkingMode`, `graphComponents`, `stateInputNames`, or `stateOutputNames`
fields.  The three stages expose only `past_key.*`/`past_value.*` inputs and
`present_key.*`/`present_value.*` outputs.  No stage declares
`attention_kv`, `recurrent_state`, or `convolution_state` input/output
families.

## Contract decision

`jobs/build-qwen-onnx-stage-manifest.py` correctly rejects this subject with
`QWEN_ONNX_SEQUENCE_POLICY_REQUIRED` (and, if that field is added without a
new export, the state-family guard rejects it).  This is an intentional
fail-closed result: the bundle is a valid diagnostic fixed-context export,
but it cannot be promoted to the Spec175 `stateful-prefill-decode-v1`
subject.  The existing standalone probe is likewise explicitly
`stateless-zero-full-context-recompute` and is not a substitute.

## Required next input

Before G5 submission, produce a new adapter-certified export whose every
stage has explicit persistent state I/O for all three families, supports
one-token autoregressive decode after prefill, and includes the frozen
chat-template/workload, graph-component, tokenizer, and stage digests.  Run
the manifest builder and local ORT contract checks first; only a passing
bundle may be staged beside the already-qualified runtime SIF.

No Tiger model allocation was requested for this preflight, and no existing
artifact was modified or deleted.

After this preflight, commit `49f90553` added the same fail-closed metadata
check to the tracked `submit.sh` path.  The previously qualified SIF/control
record remains valid as historical control evidence, but a future G5 candidate
must regenerate its source seal/SIF closure so the new pre-allocation check is
part of the submitted interface.
