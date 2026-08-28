# TigerCluster job 182490: runtime success, analyzer false negative

## Claim boundary

- Slurm job `182490` remains recorded as `FAILED (1:0)` because the original
  analyzer exited nonzero. This record is not rewritten or presented as an
  originally successful campaign.
- The three-rank execution step completed (`182490.0`, `COMPLETED (0:0)`) on
  `itiger07`, `itiger08`, and `itiger09`.
- Post-hoc analysis of the frozen evidence passes after correcting an analyzer
  predicate that required synthetic legacy markers which the runtime never emits.
- No model, SIF, Repo payload, source bundle, schedule, request ID, or remote job
  was rebuilt, regenerated, retried, or resubmitted.

## Root cause

The original analyzer required both
`LLM_PIPELINE_QWEN_STAGE_DEPENDENCY_WAIT` and
`LLM_PIPELINE_QWEN_STAGE_DEPENDENCY_READY` for ranks 1 and 2. The runtime does
not emit either marker. The unit-test fixture inserted them manually, so the
local contract test could pass without representing the deployed log contract.

The corrected predicate validates causal, request-bound evidence instead:

1. each nonzero rank receives `LLM_PIPELINE_QWEN_FULL_HIDDEN_RECEIVED` for the
   request and epoch;
2. rank 1 publishes `LLM_PIPELINE_QWEN_FULL_HIDDEN_PUBLISHED`, while rank 2
   publishes `LLM_PIPELINE_QWEN_FULL_TOKEN_PUBLISHED`;
3. produced epochs equal the 47 generated-token epochs and are a subset of the
   received epochs.

The dedicated `provider-markers-<rank>.log` is used when present because
concurrent writes can splice marker text with library diagnostics in the full
provider log.

## Preserved result

- Campaign: `spec168-campaign-v3-f3c87c4005a001189547`
- Request: `/spec168-f3c87c4005a001189547-single`
- Model: Qwen3-0.6B, digest
  `sha256:a317ec50b9a20ebf83a96379016e227dbe83c0b7116e97cfffdfc0bcee4c86db`
- Three distinct RTX 5000 Ada GPUs; CUDA on all ranks; CPU fallback count `0`
- One durable wire request; token request count `0`
- 47 generated tokens; stop reason `EOS`; nonempty complete Chinese answer
- Corrected analysis: `reanalysis.json`, SHA-256
  `20d18e04b917359e21e126608cccfec463b386e86338f786687f62696317f98f`
- Raw response: `raw/node-0/generation-raw.jsonl`, SHA-256
  `65d586e5e82fbdb99d260cb465a4e2f6f43765022d29cef9c22a39c924366753`
- Slurm accounting: `raw/sacct.txt`, SHA-256
  `901355a87f458895b9acc08b188fd5ad58e662021fc32edfcdb2156e3b559dd5`

This evidence closes the analyzer false negative. It does not establish
cold-versus-warm reuse distributions or concurrent-request behavior; those are
separate future experiments.
