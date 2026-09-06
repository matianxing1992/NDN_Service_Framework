#!/usr/bin/env bash
#SBATCH --job-name=ndnsf-spec180-yolo
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --gres=gpu:rtx_6000:1
#SBATCH --mem=96G
#SBATCH --time=00:30:00

set -euo pipefail
# The submit tree is the bundle; the functional runner binds it read-only at
# /bundle inside the sealed image.  Slurm starts the job in the submit dir.
export SPEC180_BUNDLE_DIR="$SLURM_SUBMIT_DIR"
exec "$SPEC180_BUNDLE_DIR/packaging/ndnsf-di-container/jobs/spec180/run-functional.sh"
