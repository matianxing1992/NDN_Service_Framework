#!/usr/bin/env bash
# Exact-SIF CUDA/ORT preflight for the post-assignment-fix release.
# No NFD, Provider, or network protocol process is started here.
#SBATCH --job-name=spec170-fc4e-pre
#SBATCH --partition=bigTiger
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=16G
#SBATCH --gres=gpu:1
#SBATCH --time=00:10:00
#SBATCH --exclude=itiger07

set -euo pipefail
unset APPTAINERENV_HOME SINGULARITYENV_HOME
: "${SPEC170_PROJECT_ROOT:?}"
: "${SPEC170_TOOL_ROOT:?}"
: "${SPEC170_RUNTIME_SIF:?}"
: "${SPEC170_RUNTIME_SIF_SHA256:?}"
: "${SPEC170_IDENTITY_ROOT:?}"
test "$(sha256sum "$SPEC170_RUNTIME_SIF" | cut -d' ' -f1)" = "${SPEC170_RUNTIME_SIF_SHA256#sha256:}"
scratch="/tmp/ndnsf-di-${SLURM_JOB_ID}"
mkdir -p "$scratch/evidence"
trap 'rm -rf "$scratch"' EXIT
"$SPEC170_TOOL_ROOT/preflight-compute.sh" \
  --scratch "$scratch" --gpu-type allocated --gpu-count 1
"$SPEC170_TOOL_ROOT/run-container.sh" \
  --sif "$SPEC170_RUNTIME_SIF" --sif-sha256 "$SPEC170_RUNTIME_SIF_SHA256" --gpu-count 1 \
  --project "$SPEC170_PROJECT_ROOT" --scratch "$scratch" --identity "$SPEC170_IDENTITY_ROOT" -- \
  /usr/local/bin/ndnsf-di-probe-runtime --mode allocated-gpu --output /scratch/allocated-gpu.json
python3 - "$scratch/allocated-gpu.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1], encoding="utf-8"))
assert d.get("status") == "PASS", d
assert d.get("gpu", {}).get("cpuFallback") is False, d
print(json.dumps({"status": d["status"], "gpu": d["gpu"]}, sort_keys=True))
PY
echo "SPEC170_FC4E007F_D1_RUNTIME_PREFLIGHT_PASS jobId=${SLURM_JOB_ID}"
