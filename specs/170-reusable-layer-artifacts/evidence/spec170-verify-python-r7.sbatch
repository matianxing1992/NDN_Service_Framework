#!/usr/bin/env bash
# Verify the Python modules that the D0 user driver imports from the exact r7
# SIF.  This is a read-only artifact check; it does not rebuild the image.
#SBATCH --job-name=spec170-py-r7
#SBATCH --partition=bigTiger
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=8G
#SBATCH --time=00:05:00
#SBATCH --output=/project/tma1/ndnsf-di/campaigns/spec110/spec170-py-r7/slurm-%j.out

set -euo pipefail
: "${SPEC170_RUNTIME_SIF:?}"
: "${SPEC170_RUNTIME_SIF_SHA256:?}"
test "$(sha256sum "$SPEC170_RUNTIME_SIF" | cut -d' ' -f1)" = "${SPEC170_RUNTIME_SIF_SHA256#sha256:}"
probe_home="/tmp/ndnsf-di-${SLURM_JOB_ID}/home"
mkdir -p "$probe_home"
env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  HOME=/home/$(id -un) PYTHONNOUSERSITE=1 \
  apptainer exec --cleanenv --containall --no-home --pwd / \
  --bind "$probe_home:/home/$(id -un)" "$SPEC170_RUNTIME_SIF" \
  /opt/venv/bin/python -c 'import ndnsf_distributed_inference.retry, ndnsf_distributed_inference.runtime_v1; print("SPEC170_PYTHON_V1_IMPORT_PASS")'
