#!/usr/bin/env bash
set -euo pipefail

repo=$(git rev-parse --show-toplevel)
exec python3 \
  "$repo/packaging/ndnsf-di-container/oci/layered/scripts/build-layered-local.py" \
  "$@"
