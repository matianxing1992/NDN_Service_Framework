#!/usr/bin/env bash
set -euo pipefail

# The wrapper is outside the immutable workload bundle so the candidate SIF
# and its workload remain byte-identical.  It only enables diagnostics that
# are intentionally not part of the normal workload environment.
export NDNSF_CONTROL_TIMING=1
export NDNSF_TIMELINE_TRACE=1
export NDNSF_COLLAB_ASSIGNMENT_FETCH_TRACE=1
export NDNSF_PY_COLLAB_SELECTION_TRACE=1

exec /release/spec170-runtime-edd1e096-role-evidence-python-20260817-r5/network-bundle-d2a-independent/spec170-d1-current-sif-workload.sh
