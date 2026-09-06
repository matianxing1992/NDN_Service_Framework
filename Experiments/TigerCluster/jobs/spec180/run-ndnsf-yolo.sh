#!/bin/bash
# Thin entrypoint: supervisor and bootstrap come from this same sealed tree.
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
exec /opt/venv/bin/python "$script_dir/supervise-tiger.py" "$@"
