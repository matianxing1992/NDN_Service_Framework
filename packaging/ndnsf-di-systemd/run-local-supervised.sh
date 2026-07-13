#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if [ "${1:-}" = operations ]; then
  shift
  exec python3 "$repo/tools/ndnsf-di/run_spec107_operations.py" "$@"
fi
exec python3 "$repo/tools/ndnsf-di/spec107_local_supervisor.py" "$@"
