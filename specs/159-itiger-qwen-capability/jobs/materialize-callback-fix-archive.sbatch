#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodelist=itiger07
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=22G
#SBATCH --time=00:30:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec159/materialization/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec159/materialization/slurm-%j.err

set -Eeuo pipefail
export SPEC159_RUN_ID='spec159-run-sif-archive-3ac97b8792ff302d-002'
export SPEC159_SUBMISSION_ID='spec159-submission-sif-archive-3ac97b8792ff302d-002'
export SPEC159_REPLACES_JOB='173008'
export SPEC159_OCI_REFERENCE='ghcr.io/matianxing1992/ndnsf-di@sha256:3ac97b8792ff302dba439da38756849119148e1e69dd3e7705fbb21f1f29ffc5'
export SPEC159_IMAGE_HEX='ac99ba7bc1fc56ca3205afb32cff6588268c0e966dcf64106543276dbd7a1aca'
export SPEC159_ARCHIVE='/project/tma1/ndnsf-di/images/spec159-3ac97b8792ff302d/candidate-docker-archive.tar.gz'
export SPEC159_ARCHIVE_SHA256='9ce74eff5a4b3593891156dd0c7b2921f1ddaad572ba341a3c5392f168821aea'
export SPEC159_RELEASE_ID='spec159-3ac97b8792ff302d'
exec /project/tma1/ndnsf-di/src/spec159/materialize-archive-runner.sh
