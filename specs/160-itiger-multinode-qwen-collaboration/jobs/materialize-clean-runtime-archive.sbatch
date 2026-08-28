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
export SPEC160_RUN_ID='spec160-run-sif-archive-178122a30d0a-001'
export SPEC160_SUBMISSION_ID='spec160-submission-sif-archive-178122a30d0a-001'
export SPEC160_REPLACES_JOB='173293'
exec /project/tma1/ndnsf-di/src/spec160/materialize-archive-runner.sh
