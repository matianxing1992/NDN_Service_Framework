# Spec 168 v30 Canonical Gate B

- Candidate: `20260803T231633Z-v30-closed-overlay`
- Source digest:
  `sha256:9cc623472357d0f0f52e2f6fc669f82eb072b2e6dd5c5b63f88e3bde55b1dae4`
- Runtime source bundle digest:
  `sha256:c4428cf40dbf7f3da74b050d2418a1d72c82476a69ae37cda367f4243cd333f1`
- Gate manifest:
  `results/spec168-local-gates/20260803T231633Z-v30-closed-overlay/gate-manifest.json`
- Status: `PASS`
- Elapsed: 92,230.833 ms
- Device class: `CPU_LOGIC`; this makes no CUDA claim
- Local limits: 6 GiB memory and 7 GiB memory-plus-swap on the 8 GiB host
- Request ID: `/spec168%2Fminindn%2Ftiny-qwen3%2Fv30`
- Lifecycle: 20 ordered events from `REQUEST_CREATED` through
  `RESPONSE_PUBLISHED`
- Lifecycle digest:
  `sha256:5ae8dde68585a16a654024cb2df52138ea87ee5921b274e83067c77df8b51297`
- Runtime admission digest:
  `sha256:07f1b98b2413742ba86793e0f941949024d6996acf92bb10d1fe6ea84d0a880e`
- Repository unique bytes: 78,254,966
- Generated tokens: 4
- No NFD/NLSR process remained after exit and no kernel OOM/memory-cgroup
  event was observed in the run interval.

Before the lifecycle run, the exact overlay layout was reproduced in the same
parent image under a 2 GiB limit. Python loaded the complete DI package from
the frozen bundle, imported `DIDataDependencyV2` and
`new_legacy_rollback_plan`, and passed all 24 Selection dataflow tests. This
closes the partial-package defect retained under Gate C job 182382.
