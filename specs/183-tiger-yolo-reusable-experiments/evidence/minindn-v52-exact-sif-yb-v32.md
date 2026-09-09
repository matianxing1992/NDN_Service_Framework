# Exact-SIF MiniNDN Y-B: v52 / v32 layered application

**Run:** `minindn-local-20260909-v52-yb49`
**Date:** 2026-09-09
**Scope:** one real local exact-SIF CPU YOLO inference through the maintained
MiniNDN driver

The run mounted the external APP bundle read-only at `/app` while executing
the fixed base image:

| Component | Identity |
| --- | --- |
| Apptainer | `/opt/apptainer/1.5.3/bin/apptainer` |
| Base SIF | `base-runtime-controller-version-j4-v22.sif` / `sha256:2c07a9f14d48fabd9fb58036c1634f3cc3282dd28c6470add9f8a7da0cb829b5` |
| APP manifest | `sha256:3f81b1c5203bc2f4117dd38a4c7a20ad53027cfeac20f526cb4f27d926afbf4d` |
| APP source revision | `311318f3e867689059e9c80b25e9d6aa418e4a56` |

The command used for the completed run was:

```bash
ROOT=$(readlink -f Experiments/TigerCluster/.cache/layered-base-20260909)
RUN=minindn-local-20260909-v52-yb49
OUT=Experiments/TigerCluster/results
PREP=$(sha256sum "$OUT/$RUN/public/preparation.json" | awk '{print "sha256:"$1}')
export SPEC180_RUNTIME_SIF="$ROOT/base-runtime-controller-version-j4-v22.sif"
export SPEC180_RUNTIME_APPTAINER=/opt/apptainer/1.5.3/bin/apptainer
export SPEC180_RUNTIME_APP_ROOT="$ROOT/app-controller-version-j4-v32"
export SPEC180_HOST_LIBRARY_PATH=/tmp/t008-build-root/lib:/home/tianxing/NDN/ndn-svs/build:/home/tianxing/NDN/NAC-ABE/build:/usr/local/lib
export PYTHONPATH="$PWD/NDNSF-DistributedInference:$PWD/NDNSF-DistributedRepo/pythonWrapper:$PWD/pythonWrapper"
python3 -u Experiments/TigerCluster/tools/spec183_minindn.py \
  --run-id "$RUN" --output "$OUT" \
  --profile Experiments/TigerCluster/profiles/yolo-two-node-controller-v32.json \
  --preparation-sha256 "$PREP" --case Y-B
```

The exact APP/SIF entrypoint check used the same read-only composition before
the network run:

```bash
ROOT=$(readlink -f Experiments/TigerCluster/.cache/layered-base-20260909)
AP=/opt/apptainer/1.5.3/bin/apptainer
"$AP" exec --cleanenv --bind "$ROOT/app-controller-version-j4-v32:/app:ro" \
  "$ROOT/base-runtime-controller-version-j4-v22.sif" env \
  PYTHONPATH=/app/repo/NDNSF-DistributedInference:/app/repo/examples/python \
  /opt/venv/bin/python /app/repo/examples/python/NDNSF-DistributedInference/yolo_2x2/user.py --help
```

This entrypoint check exited `0`; the Y-B command above then exercised the
same APP binary set through the registered Controller/Repo/Provider graph.

The maintained owner returned `T010_DONE` with return code `0`.  The run
started four cross-process providers and a User over MiniNDN, completed one
request, and cleaned all children and network resources.  The numerical
receipt reports `shape=[1,50,6]`, `matched=true`, and
`maxAbsError=0.0005340576171875`; the three ORT roles report real CPU compute,
load, and warmup, while Merge reports the native postprocess runner.

This is the direct local recipe to demonstrate that the fixed SIF plus APP
binary composition can perform YOLO inference.  It proves local CPU/MiniNDN
usability and exact identity binding.  It does not qualify a GPU or
TigerCluster allocation; those gates still require the same immutable
composition on the target nodes.
