# v29 canonical Gate B

## Verdict

`PASS` — real MiniNDN, bounded three-role Qwen3 fixture, canonical runtime
admission and lifecycle analysis.

- Gate manifest:
  `results/spec168-local-gates/20260803T234500Z-v29-canonical-local-gate/gate-manifest.json`
- Runtime output:
  `results/spec168-real-minindn-dev/20260803T234500Z-tiny-qwen3-v29-oci`
- Candidate:
  `results/spec168-container-candidate/20260803T234500Z-v29-canonical-local-gate`
- Source identity:
  `sha256:213a425256e599c7b76b58b6b99dce8845f961e198a018e727d8561376bb9851`
- Runtime-manifest digest:
  `sha256:f1e1670d76db8028877d6923e4a15b54782b975833bf84277ee55b0735d87397`
- Lifecycle digest:
  `sha256:c1afbbd0ba214bfebf7f00b2dc8cc6563592468718d0d9746c5205a5184ce92a`

## Measured result

- Gate elapsed time: 125,787.885 ms.
- One request ID: `/spec168%2Fminindn%2Ftiny-qwen3%2Fv29`.
- Four generated tokens and one complete Response.
- 20 accepted lifecycle observations from `REQUEST_CREATED` through one
  `RESPONSE_PUBLISHED`; one attempt and one committed plan.
- Three distinct Provider PIDs, boot epochs, roles, artifact digests, load and
  warmup completions.
- 78,254,966 unique DistributedRepo bytes.
- Adapter device class `CPU_LOGIC`; all three `cpuFallbackCount` values are 0.
- Docker limit: 6 GiB memory, 7 GiB memory-plus-swap on the 8 GiB host.
- No OOM/memory-cgroup/killed-process kernel record in the run interval; no
  host NFD/NLSR remains.

The 75 MiB content-addressed stage bundle remains in the shared artifact store.
Run-local Repo payloads, Provider caches, private keys and tokens were removed;
the v29 run directory retains only small manifests, public certificates, logs,
registration metadata and analysis (about 580 KiB).

## Remaining boundary

This result is CPU logic evidence, not CUDA or TigerCluster inference. Campaign
V2 now binds this local fixture independently from the remote Qwen3-0.6B and
large-model manifests. T004 remains open until exact-SIF Gate C passes with the
same local fixture/source identity; no Slurm inference job is yet authorized.
