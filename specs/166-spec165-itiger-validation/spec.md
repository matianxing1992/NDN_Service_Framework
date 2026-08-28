# Feature Specification: Spec 166 iTiger External Validation

## Goal

Validate the exact locally authorized Spec 165 Qwen3-0.6B workload and
candidate runtime on three physical iTiger RTX 5000 nodes, one stage per node,
without silently changing the model, workload, source, image, backend, or
failed-run identity.

## Frozen inputs

- Local authorization run: `20260731T064155Z-e3a1011e`.
- Candidate Docker image ID:
  `sha256:dcef2858c060ba0ba01903dd57ca5d3e844d1c1f9b500ed59f5dba9dd753ac47`.
- Model: `Qwen/Qwen3-0.6B`, revision
  `e6de91484c29aa9480d55605af694f39b081c455`.
- Model digest:
  `sha256:5ce2a6d5d0e96dea66cc439b6443460660cd8d99ad1ae84e7139033349851e7a`.
- Workload digest:
  `sha256:2d2aac62e9340ed401b7e7579d992f7fe652de06ee0829e0d8d0ea4c653a7ae9`.
- Workload: two prompts; one warmup and three measured generations per
  prompt; greedy decoding; exactly eight generated tokens per invocation.

## Functional requirements

- **FR-001**: Materialize the exact Docker image as one immutable SIF and
  record both identities and the SIF SHA-256.
- **FR-002**: Stage the exact workload, tokenizer/model snapshot, policy,
  runtime manifest, and three ONNX artifacts with checksums.
- **FR-003**: Run a bounded standalone same-model ONNX CUDA reference before
  the distributed candidate.
- **FR-004**: Allocate exactly three distinct RTX 5000 nodes and one GPU per
  node, with one ordered stage per node.
- **FR-005**: Require `CUDAExecutionProvider` as the primary execution
  provider. ONNX Runtime profiling MUST prove CUDA execution for the semantic
  groups matrix multiplication (`MatMul` or `FusedMatMul`), softmax
  (`Softmax`), and normalization (`ReduceMean` or the optimized
  `SimplifiedLayerNormalization`). CPU execution is permitted only when both
  the operator is in the frozen shape/control allowlist and every profiled
  input/output is a bounded `int64`/`bool` metadata tensor. An unknown CPU
  operator, missing CPU type/shape evidence, any floating or oversized CPU
  tensor, any core-compute operator on CPU, an empty profile, or a missing
  required CUDA semantic group is a terminal failure.
- **FR-006**: Use normal NDNSF Request, ACK, Selection, dependency transport,
  operation status, and Response paths with application-owned request IDs.
- **FR-007**: Retain all two warmup and six measured rows, full answers, TTFT,
  inter-token latency, total latency, tokens/s, stage timing, GPU UUID, and
  request lineage.
- **FR-008**: Use progress observation plus an immutable hard timeout; elapsed
  wall time alone must not relabel a progressing job as stalled.
- **FR-009**: Submit each preregistered job identity at most once. Preserve the
  first failure and do not auto-retry or mutate its parameters.
- **FR-010**: Promote results and checksums to project storage before scratch
  cleanup; never run inference or conversion on the login node.

## Success Criteria

- **SC-001**: Standalone reference matches all frozen reference token
  sequences.
- **SC-002**: Three distinct nodes and GPU UUIDs are recorded.
- **SC-003**: All three sessions select CUDA first and produce non-empty
  per-node execution-provider profiles. Every profiled CPU event satisfies the
  frozen operator and bounded metadata-tensor rules, every required semantic
  compute group has CUDA events, and no core-compute operator has a CPU event.
- **SC-004**: All eight distributed generations are `OK`, contain eight
  tokens, and match the frozen reference sequences.
- **SC-005**: Application and wire request IDs match for every token request.
- **SC-006**: Durable evidence passes its checksum manifest and records the
  Slurm terminal state without rewriting failures.

## Scope boundary

This is external-validity/correctness evidence for one small-model three-node
layer pipeline. It is not throughput scaling, a large-model claim, production
authority, tensor parallelism, or statistical performance proof.
