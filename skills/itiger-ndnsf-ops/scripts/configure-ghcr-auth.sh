#!/usr/bin/env bash
set -euo pipefail

registry=ghcr.io
helper=docker-credential-secretservice

command -v gh >/dev/null || { echo GH_CLI_MISSING >&2; exit 3; }
command -v docker >/dev/null || { echo DOCKER_CLI_MISSING >&2; exit 3; }
command -v "$helper" >/dev/null || {
  echo DOCKER_SECRET_SERVICE_HELPER_MISSING >&2
  exit 3
}

gh auth status -h github.com >/dev/null 2>&1 || {
  echo GITHUB_AUTH_MISSING >&2
  exit 3
}
gh auth status -h github.com 2>&1 | grep -q 'write:packages' || {
  echo GITHUB_WRITE_PACKAGES_SCOPE_MISSING >&2
  echo 'Run: GH_BROWSER=/bin/false gh auth refresh -h github.com -s write:packages' >&2
  exit 3
}

config="${DOCKER_CONFIG:-$HOME/.docker}/config.json"
python3 - "$config" <<'PY'
import json
import os
import sys
from pathlib import Path

path = Path(sys.argv[1])
path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
data = json.loads(path.read_text()) if path.exists() else {}
data.setdefault("credHelpers", {})["ghcr.io"] = "secretservice"
data.get("auths", {}).pop("ghcr.io", None)
tmp = path.with_suffix(path.suffix + ".tmp")
tmp.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
os.chmod(tmp, 0o600)
os.replace(tmp, path)
PY

user=$(gh api user --jq .login)
gh auth token | docker login "$registry" --username "$user" --password-stdin >/dev/null

python3 - "$config" <<'PY'
import json
import sys

data = json.load(open(sys.argv[1]))
assert data.get("credHelpers", {}).get("ghcr.io") == "secretservice"
entry = data.get("auths", {}).get("ghcr.io", {})
assert not entry or "auth" not in entry
PY
printf '%s' "$registry" | "$helper" get |
  python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["Username"] and d["Secret"]; print("GHCR_PERSISTENT_AUTH=PASS")'
