#!/usr/bin/env bash
# Spec183 allocation wrapper.  All release decisions happen in submit.py;
# this file only carries the already-bound arguments into the allocation.
set -euo pipefail

if [[ -z "${SLURM_JOB_ID:-}" ]]; then
  echo 'ALLOCATION_REQUIRED' >&2
  exit 78
fi
if [[ "$#" -ne 6 ]]; then
  echo 'RUN_ARGUMENTS' >&2
  exit 2
fi

bundle=$1
profile=$2
output=$3
run_id=$4
case_name=$5
operator_python=$6

# Do not accept shell fragments, relative bundle roots, or a caller-provided
# environment.  The profile validator has already checked the same values;
# these checks protect the job boundary if the script is invoked directly.
for value in "$bundle" "$profile" "$output" "$operator_python"; do
  [[ "$value" == /* && "$value" != *$'\n'* && "$value" != *:* && "$value" != *,* ]] || {
    echo 'RUN_PATH' >&2
    exit 2
  }
done
[[ "$run_id" =~ ^[a-z][a-z0-9-]{1,47}$ ]] || { echo 'RUN_ID' >&2; exit 2; }
case "$case_name" in
  local-cpu|single-node-gpu|two-node-gpu|negative-dependency) ;;
  *) echo 'RUN_CASE' >&2; exit 2 ;;
esac

exec "$operator_python" -B "$bundle/jobs/yolo/submit.py" run \
  --profile "$profile" --run-id "$run_id" --output "$output" --case "$case_name"
