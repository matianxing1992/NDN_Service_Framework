#!/bin/bash
# Resource arguments come only from the validated profile in submit.py.
set -euo pipefail
export SLURM_EXPORT_ENV=ALL
bundle=$1
profile=$2
output=$3
run_id=$4
workload=$5
exec /usr/bin/python3 "$bundle/jobs/baseline/submit.py" run \
  --profile "$profile" --output "$output" --run-id "$run_id" --workload "$workload"
