#!/usr/bin/env bash
# Build the portable external APP half of a verified base-SIF pair.
set -euo pipefail
script_dir=$(CDPATH=; cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$script_dir/build-sif-app.py" "$@"
