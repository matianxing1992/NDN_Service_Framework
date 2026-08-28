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
export SPEC160_RUN_ID='spec160-run-sif-archive-99ed026f0c1c-001'
export SPEC160_SUBMISSION_ID='spec160-submission-sif-archive-99ed026f0c1c-001'
export SPEC160_REPLACES_JOB='173303'
export SPEC160_OCI_REFERENCE='ghcr.io/matianxing1992/ndnsf-di@sha256:99ed026f0c1cb1612158c17332a9266b00dde40c49a340d1770b9b713c1dbef1'
export SPEC160_IMAGE_HEX='f6d922896d2db96cd3d2c8ece2f355aa909b594bedc47d475dd6f27771859deb'
export SPEC160_ARCHIVE='/project/tma1/ndnsf-di/images/spec160-99ed026f0c1c-v3/candidate-docker-archive.tar.gz'
export SPEC160_ARCHIVE_SHA256='2f61567ab7b02a9fce64cd92f1c03421038ec7ead9cac69a9763cf199448aa14'
export SPEC160_RELEASE_ID='spec160-99ed026f0c1c-archive'
exec /project/tma1/ndnsf-di/src/spec160/materialize-archive-runner.sh
