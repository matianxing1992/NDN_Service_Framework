#!/usr/bin/env bash
# Candidate-bound SIF materialization. Both the wrapper and its helper are
# staged under project evidence before this job is submitted. Override
# CANDIDATE_DIR and OCI_REFERENCE for a new immutable release; defaults retain
# the original 85d7aa diagnostic identity.
#SBATCH --job-name=spec170-sif-85d7aa
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --cpus-per-task=8
#SBATCH --mem=24G
#SBATCH --time=00:30:00
#SBATCH --output=/project/tma1/ndnsf-di/logs/spec170-sif-85d7aa-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/logs/spec170-sif-85d7aa-%j.err

set -euo pipefail
CANDIDATE_DIR=${CANDIDATE_DIR:-runtime-85d7aa475217cabbcfa92ad28612db99cd36e77b}
oci=${OCI_REFERENCE:-ghcr.io/matianxing1992/ndnsf-di-spec170@sha256:7ab382b75a743b6599b4250e5fb3171289e887177304b6de3759d3278fd75d0c}
stage=/project/tma1/ndnsf-di/evidence/spec170/${CANDIDATE_DIR}/materialization
final_sif=/project/tma1/ndnsf-di/releases/spec170-${CANDIDATE_DIR}/runtime.sif
final_record=/project/tma1/ndnsf-di/releases/spec170-${CANDIDATE_DIR}/materialization.json

test -r "$stage/materialize-sif-local-scratch.sh"
test -r "$stage/materialize-sif.sh"
sha256sum "$stage"/*.sh
bash -n "$stage/materialize-sif-local-scratch.sh"
sh -n "$stage/materialize-sif.sh"
bash "$stage/materialize-sif-local-scratch.sh" "$oci" "$final_sif" "$final_record"
printf 'SPEC170_RUNTIME_85D7AA_SIF_PASS job=%s sif=%s\n' "$SLURM_JOB_ID" "$final_sif"
