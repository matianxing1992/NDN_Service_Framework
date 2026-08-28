#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodelist=itiger07
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=16G
#SBATCH --gres=gpu:rtx_5000:1
#SBATCH --time=00:20:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec159/ndnsf-di/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec159/ndnsf-di/slurm-%j.err

set -Eeuo pipefail
export SPEC159_RUN_ID='spec159-run-ndnsf-qwen-7ae557604adf-001'
export SPEC159_SUBMISSION_ID='spec159-submission-ndnsf-qwen-7ae557604adf-001'
export SPEC159_REPLACES_JOB='none'
exec /project/tma1/ndnsf-di/src/spec159/ndnsf-qwen-runner.sh
