#!/usr/bin/env bash
# r6 rebuilds the exact 9678621 candidate, which includes the Python retry
# helper in the application wheel.  It uses the same pinned bases and tool
# hashes as r5; only the sealed source revision and release identity differ.
#SBATCH --job-name=spec170-gpu-r6
#SBATCH --partition=bigTiger
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=16
#SBATCH --mem=64G
#SBATCH --time=03:00:00
#SBATCH --output=/project/tma1/ndnsf-di/campaigns/spec110/spec170-9678621-build-r6/slurm-%j.out

set -euo pipefail

exec /project/tma1/ndnsf-di/staging/spec170-9678621-tools-20260816/rootless-build.sh \
  --mode full \
  --source-root /project/tma1/ndnsf-di/staging/spec170-9678621-source-sealed-20260816 \
  --project-root /project/tma1/ndnsf-di \
  --release-id spec170-runtime-9678621-gpu-20260816-r6 \
  --evidence-dir /project/tma1/ndnsf-di/campaigns/spec110/spec170-9678621-build-r6 \
  --builder-mode apptainer-sif \
  --builder-oci quay.io/buildah/stable@sha256:8570703f0feb3f39d180e932a2ec8e350ee860790062a5ecd5a3b3ac51f337c5 \
  --probe-base docker.io/library/ubuntu@sha256:8feb4d8ca5354def3d8fce243717141ce31e2c428701f6682bd2fafe15388214 \
  --foundation-base docker.io/library/ubuntu@sha256:8feb4d8ca5354def3d8fce243717141ce31e2c428701f6682bd2fafe15388214 \
  --gpu-build-base docker.io/nvidia/cuda@sha256:f18cf1a9ac2842e59f13b0d0729594da8cbd68cadd2379308cdd98c0374dbd80 \
  --gpu-runtime-base docker.io/nvidia/cuda@sha256:a6a8417cb56c9a5d30c4d8c78ad18bc9b75ffe4453fe1c04b3149b3741518b06
