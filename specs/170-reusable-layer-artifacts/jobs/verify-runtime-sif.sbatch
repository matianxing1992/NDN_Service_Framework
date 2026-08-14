#!/usr/bin/env bash
# Verify an already promoted candidate-bound SIF without rebuilding it.
# Set CANDIDATE_DIR and OCI_REFERENCE through sbatch --export.
#SBATCH --job-name=spec170-sif-verify
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --cpus-per-task=2
#SBATCH --mem=4G
#SBATCH --time=00:10:00
#SBATCH --output=/project/tma1/ndnsf-di/logs/spec170-sif-verify-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/logs/spec170-sif-verify-%j.err

set -euo pipefail
CANDIDATE_DIR=${CANDIDATE_DIR:?CANDIDATE_DIR required}
OCI_REFERENCE=${OCI_REFERENCE:?OCI_REFERENCE required}
stage=/project/tma1/ndnsf-di/evidence/spec170/${CANDIDATE_DIR}/materialization
sif=/project/tma1/ndnsf-di/releases/spec170-${CANDIDATE_DIR}/runtime.sif
record=/project/tma1/ndnsf-di/releases/spec170-${CANDIDATE_DIR}/materialization.json
test -r "$stage/materialize-sif.sh"
test -s "$sif"
test -s "$record"
sha256sum "$stage/materialize-sif.sh" "$sif" "$record"
bash -n "$stage/materialize-sif.sh"
bash "$stage/materialize-sif.sh" --oci-reference "$OCI_REFERENCE" --sif "$sif" --record "$record"
printf 'SPEC170_RUNTIME_SIF_VERIFY_PASS candidate=%s job=%s sif=%s\n' "$CANDIDATE_DIR" "$SLURM_JOB_ID" "$sif"
