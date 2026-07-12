#!/bin/sh
set -eu

usage() {
  echo "usage: $0 --output DIR --release-id ID [--source-commit COMMIT] [--artifact SOURCE:DEST]" >&2
  exit 2
}

output=
release_id=
source_commit=
artifacts=
while [ "$#" -gt 0 ]; do
  case "$1" in
    --output) output=$2; shift 2 ;;
    --release-id) release_id=$2; shift 2 ;;
    --source-commit) source_commit=$2; shift 2 ;;
    --artifact) artifacts="${artifacts}${artifacts:+
}$2"; shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$output" ] && [ -n "$release_id" ] || usage
[ ! -e "$output" ] || { echo "release output already exists: $output" >&2; exit 1; }
case "$release_id" in *[!A-Za-z0-9._-]*|'') echo "invalid release id" >&2; exit 2 ;; esac

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
[ -n "$source_commit" ] || source_commit=$(git -C "$repo" rev-parse HEAD)
mkdir -p "$output/bin" "$output/share/ndnsf-di-systemd"
cp -a "$repo/packaging/ndnsf-di-systemd/units" "$output/share/ndnsf-di-systemd/"
cp -a "$repo/packaging/ndnsf-di-systemd/config" "$output/share/ndnsf-di-systemd/"

old_ifs=$IFS
IFS='
'
for item in $artifacts; do
  [ -n "$item" ] || continue
  source=${item%%:*}
  destination=${item#*:}
  [ "$source" != "$destination" ] || { echo "artifact must be SOURCE:DEST" >&2; exit 2; }
  case "$destination" in /*|*../*) echo "unsafe artifact destination: $destination" >&2; exit 2 ;; esac
  mkdir -p "$output/$(dirname -- "$destination")"
  cp -a "$source" "$output/$destination"
done
IFS=$old_ifs

(
  cd "$output"
  find . -type f ! -name SHA256SUMS ! -name release-manifest.json -print0 |
    sort -z | xargs -0 sha256sum > SHA256SUMS
)
python3 - "$output" "$release_id" "$source_commit" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])
entries = []
for line in (root / "SHA256SUMS").read_text(encoding="utf-8").splitlines():
    digest, name = line.split(None, 1)
    entries.append({"path": name.lstrip("*"), "sha256": digest})
manifest = {
    "schema": "ndnsf-di-release-manifest-v1",
    "releaseId": sys.argv[2],
    "sourceCommit": sys.argv[3],
    "compatibility": {
        "profile": "ndnsf-di-deployment-v1",
        "plan": "runtime-v1",
        "executionEvidence": "ndnsf-di-execution-evidence-v1",
        "telemetry": "ndnsf-di-provider-telemetry-v1",
    },
    "artifacts": entries,
}
(root / "release-manifest.json").write_text(
    json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY
echo "$output"
