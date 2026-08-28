# Spec 166 Standalone and Three-Node Results — 2026-07-31

## Accepted standalone gate

- Job: `181085` / `spec166-standalone-qwen3-005`
- Source: `source-005`; manifest SHA-256
  `f2bd6c0887ddfd90356a7671117b1f11ad71d7919da1bc3d00cecab36b8fdc55`
- Slurm: `COMPLETED 0:0`, 56 s, `itiger07`
- Result: 8/8 exact eight-token rows; three CUDA-first ONNX profiles; all
  profile-policy-v2 verdicts PASS.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec166/standalone/spec166-standalone-qwen3-005`
- Integrity: evidence checksum manifest passes.

## Preserved three-node attempts

Each row is a separate, immutable submission. None was retried in place.

| Job | Submission | Source | State | Elapsed | Result |
|---|---|---|---|---:|---|
| `181086` | `spec166-multinode-qwen3-001` | `source-005` | `FAILED 13:0` | 0:52 | `multinode-rank.sh` lacked execute permission |
| `181088` | `spec166-multinode-qwen3-002` | `source-006` | `FAILED 2:0` | 0:30 | host wrapper referenced container-only `/source/nfd.conf.in` |
| `181089` | `spec166-multinode-qwen3-003` | `source-007` | `FAILED 1:0` | 3:45 | sealed SIF `APPClient.distributed_inference()` lacked `request_id` |
| `181091` | `spec166-multinode-qwen3-004` | `source-008` | `FAILED 6:0` | 4:39 | API overlay omitted `compatibility/manifest.json` |
| `181092` | `spec166-multinode-qwen3-005` | `source-009` | `FAILED 1:0` | 3:57 | 8/8 requests succeeded; post-run gate found bootstrap-token residue before EXIT cleanup |
| `181094` | `spec166-multinode-qwen3-006` | `source-010` | `FAILED 1:0` | 2:37 | 8/8 requests succeeded; ONNX timing lacked CUDA/digest/Data-name evidence |
| `181095` | `spec166-multinode-qwen3-007` | `source-011` | `FAILED 1:0` | 4:26 | lineage data was correct, but native and Python stdout writes corrupted Stage 2 evidence lines |
| `181096` | `spec166-multinode-qwen3-008` | `source-012` | `COMPLETED 0:0` | 5:02 | all execution and evidence gates passed |

The failed directories remain under their `.partial` submission names in
`/project/tma1/ndnsf-di/evidence/spec166/multinode`. The successful directory
was promoted only after the analyzer returned PASS.

## Accepted three-node result

- Job: `181096` / `spec166-multinode-qwen3-008`
- Source: `source-012`; manifest SHA-256
  `a7044a2332cad6a7e31a85d402257988d83310698b4b97428d5688745adc829a`
- Candidate SIF SHA-256:
  `e82d5d4b9cedacb2cb60451d7ecdf624732953359bd680fe235ba8db44c2f45a`
- Allocation: `itiger07`, `itiger08`, `itiger09`; one distinct NVIDIA RTX
  5000 Ada Generation UUID per Stage.
- Workload: two real prompts; one warmup plus three measured repetitions per
  prompt; eight new tokens per generation.
- Correctness: 8/8 `OK`, 8/8 exact reference matches, and complete decoded
  answers/token sequences retained.
- Execution: 64 timing records per Stage; 192/192 `cuda:0`; 192/192 no CPU
  fallback.
- Lineage: 64 unique request IDs on every Stage, identical sets across Stages.
- Dependency integrity: every Stage 0 output digest equals the corresponding
  Stage 1 input digest; every Stage 1 output digest equals the corresponding
  Stage 2 input digest. Both intermediate Data names are nonempty.
- Security: input, source, SIF, and evidence checksums pass; no bootstrap-token
  file or private key remains in promoted evidence.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec166/multinode/spec166-multinode-qwen3-008`

## Measured distribution

Statistics below cover the six measured rows only; the two warmups are retained
but excluded.

| Metric | Minimum | Median | P95 / maximum |
|---|---:|---:|---:|
| TTFT | 67.20 ms | 103.70 ms | 106.63 ms |
| Total generation latency | 647.17 ms | 704.82 ms | 807.32 ms |
| Throughput | 9.91 tokens/s | 11.35 tokens/s | 12.36 tokens/s |

With six observations, the nearest-rank P95 equals the maximum. These values
are descriptive acceptance evidence, not a statistically powered performance
comparison.

## Full answer retention

The promoted `node-0/generation.jsonl` retains every decoded answer, generated
token ID, TTFT, per-token latency, total latency, throughput, plan ID,
workload/model digests, and all 64 token-step request IDs. For prompt 1 the
accepted continuation begins `Immutable model identity ensures that the
model's`; for prompt 2 it begins `Progress-driven deadlines distinguish slow
work from a`. All repetitions match their frozen references exactly.

## Preventing recurrence

Future local deployment gates must validate the packaged artifact, not just
source-level mocks. At minimum they must exercise executable modes, host versus
container paths, the exact SIF API signature, package-data closure, credential
cleanup ordering, ONNX request-ID propagation, mandatory structured evidence
fields, and one-write log atomicity before TigerCluster submission.
