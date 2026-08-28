#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=22G
#SBATCH --time=00:30:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec160/materialization/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec160/materialization/slurm-%j.err

set -Eeuo pipefail
export SPEC160_RUN_ID='spec160-run-sif-archive-d9a7e3a0a30c-001'
export SPEC160_SUBMISSION_ID='spec160-submission-sif-archive-d9a7e3a0a30c-001'
export SPEC160_REPLACES_JOB='173315'
export SPEC160_OCI_REFERENCE='ghcr.io/matianxing1992/ndnsf-di@sha256:d9a7e3a0a30c21366cd5bf532402ba2c01c5702e33dbc53c02823aed028e09c4'
export SPEC160_IMAGE_HEX='debcccc2f27609882b056ce433b35224c57526b7f89801b856a0db5bc6867d3a'
export SPEC160_ARCHIVE='/project/tma1/ndnsf-di/images/spec160-d9a7e3a0a30c-v4/candidate-docker-archive.tar.gz'
export SPEC160_ARCHIVE_SHA256='20b0dbd64c7ab630658dd8c97725835a4e2454a099cd32c85d5eb2b8ae78de15'
export SPEC160_RELEASE_ID='spec160-d9a7e3a0a30c-archive'
exec /project/tma1/ndnsf-di/src/spec160/materialize-archive-runner.sh
