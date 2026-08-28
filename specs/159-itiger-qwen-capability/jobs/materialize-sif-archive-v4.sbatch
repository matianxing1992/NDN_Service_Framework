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
export SPEC159_RUN_ID='spec159-run-sif-archive-a0e58822fe127594-005'
export SPEC159_SUBMISSION_ID='spec159-submission-sif-archive-a0e58822fe127594-005'
export SPEC159_REPLACES_JOB='172766'
exec /project/tma1/ndnsf-di/src/spec159/materialize-archive-runner.sh
