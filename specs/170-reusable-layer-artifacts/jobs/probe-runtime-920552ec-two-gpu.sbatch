#!/usr/bin/env bash
# Spec170 D2 preflight only: verify exact SIF visibility of two allocated GPUs.
# This is not the D2a logical-role execution gate.
# Override CANDIDATE_DIR and SIF_SHA256 through sbatch --export.
#SBATCH --job-name=spec170-d2-topology
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --gres=gpu:rtx_5000:2
#SBATCH --cpus-per-task=4
#SBATCH --mem=12G
#SBATCH --time=00:10:00
#SBATCH --output=/project/tma1/ndnsf-di/logs/spec170-d2-topology-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/logs/spec170-d2-topology-%j.err

set -euo pipefail
CANDIDATE_DIR=${CANDIDATE_DIR:?CANDIDATE_DIR required}
SIF_SHA256=${SIF_SHA256:?SIF_SHA256 required}
sif=/project/tma1/ndnsf-di/releases/spec170-${CANDIDATE_DIR}/runtime.sif
test -s "$sif"
actual=$(sha256sum "$sif" | cut -d' ' -f1)
test "$actual" = "${SIF_SHA256#sha256:}"
echo "SPEC170_D2_TOPOLOGY_HOST_VISIBLE"
nvidia-smi --query-gpu=uuid,name,driver_version,memory.total --format=csv,noheader
apptainer exec --nv --cleanenv --containall --home /tmp --pwd /tmp \
  --env CUDA_VISIBLE_DEVICES="${CUDA_VISIBLE_DEVICES:-}" "$sif" \
  python3 - <<'PY'
import json
import os
import subprocess
import torch
import onnxruntime as ort

visible = os.environ.get("CUDA_VISIBLE_DEVICES", "")
rows = subprocess.run(
    ["nvidia-smi", "--query-gpu=uuid,name,driver_version,memory.total",
     "--format=csv,noheader"],
    check=True, text=True, capture_output=True).stdout.splitlines()
count = torch.cuda.device_count()
if count != 2 or len(rows) != 2:
    raise SystemExit(f"SPEC170_D2_TOPOLOGY_FAIL count={count} rows={len(rows)}")
if "CUDAExecutionProvider" not in ort.get_available_providers():
    raise SystemExit("SPEC170_D2_TOPOLOGY_FAIL missing CUDAExecutionProvider")
print(json.dumps({
    "status": "PASS",
    "cudaVisibleDevices": visible,
    "torchDeviceCount": count,
    "ortProviders": ort.get_available_providers(),
    "devices": rows,
}, sort_keys=True))
PY
echo "SPEC170_D2_TOPOLOGY_PREFLIGHT_PASS candidate=${CANDIDATE_DIR} job=${SLURM_JOB_ID}"
