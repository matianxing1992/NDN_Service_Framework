# Job 182508: partial cold/warm campaign and bf16 token tie

Job 182508 was the only formal submission for immutable campaign
`spec168-campaign-v3-9b47b2cb9c8d106587f3`. It reused the sealed SIF,
Qwen3-0.6B stage artifacts, prompt schedule, and content-addressed Repository
payload. No model, foundation image, or payload was rebuilt.

## Retained outcome

- Slurm: `FAILED`, exit `1:0`, elapsed `00:44:29`, nodes `itiger07-09`.
- The first 18 invocations completed authenticated three-stage CUDA inference
  with full answers: three prompts, each with one warmup and five measured
  invocations.
- Invocation 19 (`stage-timeout`, warmup) completed inference and returned a
  57-token EOS answer, but the user-side exact-reference oracle stopped the
  campaign with `TOKEN_MISMATCH expected=63 actual=57`.
- No automatic retry or replacement campaign was submitted. The missing 11
  schedule rows remain missing and T011 remains incomplete.

The compact evidence retained here contains the frozen manifests, all 19 raw
generation rows, terminal state, and request/Repository bindings. The original
large Provider and Repository logs remain on TigerCluster under:

`/project/tma1/ndnsf-di/evidence/spec168/tiger-small-repeated/spec168-campaign-v3-9b47b2cb9c8d106587f3-FAILED-job-182508`

## Primary failure boundary

The first token divergence is index 4. The pinned single-GPU reference selected
token `104399`; the distributed path selected token `34859`. Both outputs are
well-formed Chinese answers, but semantic plausibility alone is not acceptance
evidence.

The bounded local reconstruction and exact-SIF RTX 5000 diagnostic Job 182509
show that the full-model logits for both tokens are `17.75`, with a top-two
margin of `0.0`. Full-model and staged execution can therefore choose different
argmax indices at a bf16 tie while remaining numerically equivalent. Job 182509
classifies the event as `NUMERICALLY_EQUIVALENT_DIVERGENCE`; it does not rewrite
Job 182508 from FAILED to PASS.

## Additional deployment findings

- The first cold invocation took `201459.323 ms`.
- The next 17 successful invocations had median `125060.397 ms`, range
  `124357.385-126733.504 ms`.
- All 19 invocations spent approximately `120000 ms` collecting ACKs. Warm
  median TTFT was `120173.856 ms`, so the fixed ACK window hides most Provider
  model-residency reuse benefit.
- All 19 planning inputs reported `catalogCount=0`; all decisions and sample
  rows were classified `GENERATED`, even though the long-lived Provider
  processes reused already loaded stage runners. This proves process-local
  reuse, but does not validate ACK-visible GPU residency or
  `PreSplitFirstStrategy` cache preference.

## Rework-control decision

Do not retry this campaign identity. Before a replacement candidate is
admitted:

1. make the MiniNDN/exact-container schedule include the problematic real
   `stage-timeout` prompt and a bf16 near-tie oracle;
2. distinguish exact topology-repeatability from cross-topology numerical
   equivalence, backed by a pinned full-vs-staged logits diagnostic;
3. require ACK snapshots to expose actual disk/RAM/GPU residency and require
   the planner to record a nonzero catalog/cache decision on warm requests;
4. measure or bound ACK closure independently from execution so a 120-second
   collection window cannot masquerade as warm inference cost;
5. freeze a new source identity, rerun Gates A-C, and submit at most one new
   formal campaign.

Early ACK closure, soft planning, and higher request concurrency belong to the
Page 18 advisor-discussion plan. They are not claimed as current behavior.
