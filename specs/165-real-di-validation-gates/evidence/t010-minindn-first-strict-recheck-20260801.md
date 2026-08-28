# T010 Strict MiniNDN-First Recheck — 2026-08-01

Status: PASS for the Gate-B MiniNDN subset; external authorization held until
the full aggregate is rerun against this source revision.

Command:

```bash
python3 Experiments/NDNSF_DI_Run_Local_Deployment_Gates.py \
  --gate minindn \
  --output-root results/spec165-minindn-first \
  --reuse-prepared-run \
  results/spec165-local-gates/20260731T074249Z-1edd6ad0
```

NLSR result: `results/spec165-minindn-first/20260801T204709Z-28c987c4`.
Static-route result: `results/spec165-minindn-first-static/20260801T205214Z-e2281bb3`.

Both runs used the explicit `Experiments/Topology/AI_Lab.conf` topology, real
MiniNDN/NFD, three Provider processes on separate hosts, and the
content-addressed Qwen3-0.6B ONNX stage artifacts. One used NLSR and the other
used explicit static routes. The strict launcher rejected a negative-control
`--runtime fake` invocation before starting MiniNDN.

Evidence summary:

- aggregate subset: `NDNSF_SPEC165_LOCAL_GATE_PASS`;
- two prompts, two warmups, six measured requests;
- eight generated tokens per invocation with complete answer/timing/lineage;
- Selection covered `/LLM/Pipeline/Stage/0`, `/1`, and `/2`;
- all three stages emitted ONNX artifact-readiness and execution markers;
- backend: explicit CPU, `cpuFallback=true` is expected for this profile;
- `tigerClusterSubmitted=false`.

This proves the local control/data path and real model-stage execution. It does
not claim GPU performance, Docker equivalence, or TigerCluster success. Run the
full Gate A-D plus candidate-container aggregate after any harness/source
change before reopening external validation.

## Current-source aggregate and container blocker

The required current-source aggregate was executed with the same reused,
content-addressed workload:

`results/spec165-minindn-first-full/20260801T205752Z-7bc46c27`

Gates A, B, and D passed. Gate C did not. Its first failure was a container
path defect: the host absolute `AI_Lab.conf` path was not visible inside the
`/workspace` mount and MiniNDN reported `No section: 'nodes'`; the launcher now
maps that path to `/workspace/Experiments/Topology/AI_Lab.conf`.

A focused Gate-C rerun with the corrected path and a bounded 5-second
candidate ACK window is recorded at
`results/spec165-minindn-first-container/20260801T211233Z-4d951ba3`. It used
the same model/workload/image and real MiniNDN/NFD, but the CPU ONNX runtime in
`ndnsf-di:spec165-minindn-gate` became progressively slower under the three
provider processes. The user process hit its 630-second internal wait with
only four completed samples (one warmup and three measured); `oomKilled=false`.

The CUDA-wheel result is retained as a blocking candidate-runtime finding for
that image. It is not a model, NDNSF collaboration, or TigerCluster result.

## CPU candidate resolution and full aggregate — 2026-08-01

A CPU-only ONNX Runtime overlay was built from the same candidate parent using
`packaging/ndnsf-di-container/oci/Dockerfile.spec165-minindn-cpu-gate` as
`ndnsf-di:spec165-minindn-cpu-gate`, image digest
`sha256:11200f32ce8fc037152f9590bb0e65958642d6cbd9a3b6c14e3e94abb5c962c0`.
The overlay exposes `CPUExecutionProvider` without CUDA fallback; NDNSF/NDN,
MiniNDN, NFD, source mounts, model snapshot, and workload bytes are unchanged.

Focused Gate C passed at
`results/spec165-minindn-first-container-cpu/20260801T212933Z-412b76d8`.
The complete current-source aggregate passed at
`results/spec165-minindn-first-full-cpu/20260801T213143Z-1d7b91c8`:

- Gates A, B, C, and D: PASS;
- two warmups and six measured samples in both real environments;
- eight tokens per sample, all measured samples OK;
- image/model/workload identities matched;
- `externalValidationAuthorized=true`, `tigerClusterSubmitted=false`.

This closes the local CPU and candidate-container gate. It does not claim GPU
performance or TigerCluster success. A separate generic GPU capability probe
(`181812`, `itiger07`) passed without loading Qwen or starting NDNSF-DI; its
scope and evidence are recorded in
`specs/166-spec165-itiger-validation/evidence/gpu-capability-preflight-181812.md`.
The next external step is a clean current-source CUDA candidate release and
SIF materialization, followed by a candidate-bound standalone GPU preflight
using this immutable identity set.
