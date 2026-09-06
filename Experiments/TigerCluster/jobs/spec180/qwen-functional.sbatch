#!/usr/bin/env bash
#SBATCH --job-name=ndnsf-spec180-qwen
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --gres=gpu:rtx_6000:3
#SBATCH --mem=96G
#SBATCH --time=01:00:00

set -euo pipefail
cd /bundle
exec /bundle/packaging/ndnsf-di-container/jobs/spec180/run-functional.sh
