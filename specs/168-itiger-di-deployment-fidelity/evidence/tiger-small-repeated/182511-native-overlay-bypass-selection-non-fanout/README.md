# Job 182511: native overlay bypass repeated compact-Selection non-fanout

## Frozen identity and outcome

- Slurm job: `182511`
- Campaign: `spec168-campaign-v3-d79f87295ba563a87a44`
- Candidate: `20260804T151500Z-v82-multirequest-cache-reuse`
- Terminal state: `FAILED`, rank 0 exit 1; ranks 1/2 later timed out at the
  campaign barrier. This identity is closed and is not retried.
- Remote archive:
  `/project/tma1/ndnsf-di/evidence/spec168/tiger-small-repeated/spec168-campaign-v3-d79f87295ba563a87a44-FAILED-job-182511`

## Last verified checkpoint

The User published one Request, collected three positive ACKs, closed ACK
collection, generated the dependency graph and placement, published all three
Repository artifacts, attached three role assignments, and committed final
Selection. Provider 0 received Selection, fetched its 594,357,850-byte shard in
6.213 seconds with 78,205 delivered segments and zero retransmitted bytes,
loaded and warmed CUDA with zero CPU fallback, executed Stage 0, and published
its dependency output. Providers 1 and 2 recorded their positive ACKs but no
LLM Selection observation, status, model fetch, runtime readiness, or handler
execution. Stage 0 therefore timed out waiting for epoch-0 downstream output.

## Root cause

This is the same lifecycle boundary retained for Job 182413, but Job 182511
also proves why the earlier repair did not reach the runtime. The v82 source
bundle contains native core
`sha256:50c059bef41a9f66ee898e50a9f420430a4bb85cd432406d9e21116ace5bf904`,
whose binary contains `NDNSF_SELECTION_PROVIDER_PROJECTION` and
`SELECTION_TARGETED_PREFETCH_ISSUED`. The formal three-node rank script copied
Python packages itself and preserved the parent SIF's compiled extension and
core instead of invoking the complete Spec 168 overlay entrypoint. The mapped
SIF core was
`sha256:51027456e889ec256bca2a1d28d8baff0d8cfa6d85d2808ec06d62b31ccf85ed`
and contains neither marker. Runtime logs consequently show one 7,803-byte
compact LLM Selection and no provider-specific projection markers.

Repository throughput, planning, CUDA capacity, and model execution are not the
primary failure. The ownership boundary is the Spec 168 deployment launcher and
candidate-admission contract: source and SIF digests were each verified, but
the native control-plane binary actually mapped by the formal path was not.

## Repair and prevention contract

`spec168-three-node-rank-inner.sh` now delegates to the same
`spec168-overlay-entrypoint.sh` used by exact-container Gate C. Formal ranks set
`SPEC168_REQUIRE_SELECTION_FANOUT_ABI=1`; startup fails before publishing a
Request unless the candidate native core and pybind extension are mapped and
the core contains provider-projection, targeted-prefetch, and status support.
The focused launch-contract tests and a frozen-container import check must pass
under the candidate closure. A bounded three-node no-model control-plane canary
must then prove per-Provider Selection delivery and acceptance before another
Qwen campaign is admitted. This canary is not a model-readiness barrier and
does not change data-driven role execution.
