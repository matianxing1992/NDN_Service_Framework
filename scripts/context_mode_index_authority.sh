#!/usr/bin/env bash
set -Eeuo pipefail

# Re-index only the repository-backed Context Mode authority files required by
# context_mode_guard.py.  This is intentionally separate from the guard: the
# guard is fail-closed and must never write a Context Mode database.
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
context_mode_bin="${CONTEXT_MODE_BIN:-context-mode}"
project_root="${CONTEXT_MODE_PROJECT_ROOT:-$root}"

active_dir=$(python3 - "$project_root/.specify/feature.json" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
value = json.loads(path.read_text(encoding="utf-8"))["feature_directory"]
if not isinstance(value, str) or not value.strip():
    raise SystemExit("feature_directory must be a non-empty string")
feature = (path.parent.parent / value).resolve()
root = path.parent.parent.resolve()
try:
    feature.relative_to(root)
except ValueError:
    raise SystemExit("feature_directory escapes project root")
print(feature)
PY
)

index() {
  local path=$1
  local source=$2
  "$context_mode_bin" index "$path" \
    --project "$project_root" \
    --source "$source"
}

index "$project_root/.specify/feature.json" \
  "NDNSF active feature pointer"
index "$project_root/.specify/memory/context-mode-project.md" \
  "NDNSF project context anchor"
index "$project_root/.specify/memory/context-mode.md" \
  "NDNSF Context Mode operating guide"
for filename in spec.md plan.md tasks.md; do
  index "$active_dir/$filename" \
    "NDNSF active feature $(basename "$active_dir"):$active_dir/$filename"
done

python3 "$project_root/scripts/context_mode_guard.py" \
  health --project-root "$project_root"
