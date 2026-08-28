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
export SPEC160_RUN_ID='spec160-run-sif-operation-status-af0c3a3ce357-001'
export SPEC160_SUBMISSION_ID='spec160-submission-sif-operation-status-af0c3a3ce357-001'
export SPEC160_REPLACES_JOB='173319'
export SPEC160_OCI_REFERENCE='docker-archive:ndnsf-di@sha256:af0c3a3ce357f9fb5977dd1d3bad0b71d5ae2237b6a23675eccc6f4df476fe3b'
export SPEC160_IMAGE_HEX='af0c3a3ce357f9fb5977dd1d3bad0b71d5ae2237b6a23675eccc6f4df476fe3b'
export SPEC160_ARCHIVE='/project/tma1/ndnsf-di/images/spec160-af0c3a3ce357-v1/candidate-docker-archive.tar.gz'
export SPEC160_ARCHIVE_SHA256='65074a6fea4c2f85f32f6fbe24c70b3529167a5cb1c582d55026b7ef88e8028f'
export SPEC160_RELEASE_ID='spec160-af0c3a3ce357-operation-status'
exec /project/tma1/ndnsf-di/src/spec160/materialize-archive-runner.sh
