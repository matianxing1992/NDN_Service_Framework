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
export SPEC160_RUN_ID='spec160-run-sif-archive-630137257e64-001'
export SPEC160_SUBMISSION_ID='spec160-submission-sif-archive-630137257e64-001'
export SPEC160_REPLACES_JOB='173293'
export SPEC160_OCI_REFERENCE='ghcr.io/matianxing1992/ndnsf-di@sha256:630137257e64776c0126c959cbad159fca6b184bf479ef4e007759d8056161cf'
export SPEC160_IMAGE_HEX='7156c0f0cc955ce42e60fbdb27a20904fcc3e8fefb6cf5354b73b4014bb40248'
export SPEC160_ARCHIVE='/project/tma1/ndnsf-di/images/spec160-630137257e64/candidate-docker-archive.tar.gz'
export SPEC160_ARCHIVE_SHA256='7c9b2918b2c64d8406f8e9a7de00aa8d9d51efe1b20f84120a122d6b9201bf59'
export SPEC160_RELEASE_ID='spec160-630137257e64-archive'
exec /project/tma1/ndnsf-di/src/spec160/materialize-archive-runner.sh
