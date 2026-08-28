#!/usr/bin/env bash
# D1 wrapper around the candidate-bound single-provider workload. The explicit
# role-to-Provider map is required for one Provider that owns all four roles.
set -euo pipefail
src=/release/spec170-runtime-07738d70-gpu-20260816-r8b/network-bundle-d1-cuda/tiger-native-single-provider-workload.sh
bundle=/scratch/network-bundle-d1-cuda
dst="$bundle/tiger-native-single-provider-explicit.sh"
test -r "$src"
mkdir -p "$bundle"
for item in /release/spec170-runtime-07738d70-gpu-20260816-r8b/network-bundle-d1-cuda/*; do
  ln -s "$item" "$bundle/$(basename "$item")"
done
python3 - "$src" "$dst" <<'PY'
from pathlib import Path
import sys
src, dst = map(Path, sys.argv[1:])
text = src.read_text(encoding="utf-8")
needle = "  --requests 1 \\\n"
replacement = needle + (
    "  --role-provider-preference \\\n"
    "  '/Backbone=>/NDNSF-DI/Tracer/provider/single;"
    "/Head/Shard/0=>/NDNSF-DI/Tracer/provider/single;"
    "/Head/Shard/1=>/NDNSF-DI/Tracer/provider/single;"
    "/Merge=>/NDNSF-DI/Tracer/provider/single;' \\\n"
)
if text.count(needle) != 1:
    raise SystemExit("D1_WORKLOAD_REQUEST_MARKER_INVALID")
dst.write_text(text.replace(needle, replacement), encoding="utf-8")
PY
chmod 0755 "$dst"
exec "$dst"
