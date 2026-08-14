# TigerCluster exact-SIF static probe (2026-08-14)

## Candidate

- Slurm materialization job: `189255`, node `itiger05`, exit `0`.
- OCI reference: `ghcr.io/matianxing1992/ndnsf-di-spec170@sha256:94ce0cc847d453df90fc1aab74fade597f45e3199274ad782094fb45dd9bf916`.
- SIF: `/project/tma1/ndnsf-di/releases/spec170-runtime-e23d759bb61159c8b3093e599fe301599d8c043f/runtime.sif`.
- SIF SHA-256: `sha256:525c4b890c4012d3f36653d0209f7decec508635818e5b0829250ef06d012af1`.
- Apptainer: `1.5.3-1.el9`.

## Final probe

Slurm job `189260` ran on `itiger05` with no GRES and no `--nv`:

```text
apptainer exec --cleanenv --containall --home /tmp --pwd /tmp \
  --env CUDA_VISIBLE_DEVICES= runtime.sif \
  /usr/local/bin/ndnsf-di-probe-runtime --mode static
```

Exit code was `0`, and the probe reported `status: PASS`. It found the native
`App_ServiceController`, `di-native-provider`, `nfd`, and `nfdc` binaries and
all required Python modules, including `ndnsf`,
`ndnsf_distributed_inference`, the app/core/sdk/planner/ONNX/Qwen modules,
`torch 2.6.0+cu124`, `onnxruntime 1.20.0`, and `transformers 4.48.2`.
`modelWeightsIncluded: false` is expected for this image-level probe.

## Preserved failed launch attempts

- `189256`: wrapper quoting error; exit `2` before probe launch.
- `189257`: container default working directory `/home/tma1` did not exist;
  NDN initialization attempted to create read-only `/home/tma1/.ndn`; exit `6`.
- `189258`: `/scratch` was requested as `--pwd` without a container bind;
  exit `127`.
- `189259`: cluster Apptainer HOME override still routed NDN initialization to
  read-only `/home/tma1/.ndn`; exit `6`.

These are retained as launch-environment diagnostics. They do not invalidate
the exact SIF digest or the successful `189260` static probe. They also do not
constitute Gate C or D0 completion.
