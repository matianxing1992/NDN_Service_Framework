#!/usr/bin/env bash
# Spec186 scheduler payload.  The submit.py boundary supplies the effective
# candidate and run identity; this file never discovers host paths implicitly.
set -euo pipefail
: "${SPEC186_RUN_ID:?SPEC186_RUN_ID is required}"
: "${SPEC186_CANDIDATE:?SPEC186_CANDIDATE is required}"
: "${SPEC186_EFFECTIVE_CONFIG:?SPEC186_EFFECTIVE_CONFIG is required}"
echo "$SPEC186_EFFECTIVE_CONFIG"
if [[ "${SPEC186_EXECUTE:-0}" != "1" ]]; then
  echo "SPEC186_SCHEDULER_PAYLOAD_READY run=${SPEC186_RUN_ID}"
  exit 0
fi
python3 - "$SPEC186_EFFECTIVE_CONFIG" <<'PY'
import json
from pathlib import Path
import sys

try:
    config = json.loads(sys.argv[1])
    run_root = Path(config["runRoot"])
except (KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
    raise SystemExit("SPEC186_EFFECTIVE_CONFIG_INVALID") from exc
if not run_root.is_absolute() or ".." in run_root.parts:
    raise SystemExit("SPEC186_RUN_ROOT_INVALID")
(run_root / "evidence").mkdir(parents=True, exist_ok=True)
(run_root / "state").mkdir(parents=True, exist_ok=True)
PY
python3 - "$SPEC186_EXEC_ARGV" "${SPEC186_EXEC_ENV:-}" <<'PY'
import json
import os
import re
import sys

argv = json.loads(sys.argv[1])
if not isinstance(argv, list) or not argv or any(not isinstance(item, str) or not item for item in argv):
    raise SystemExit("SPEC186_EXEC_ARGV_INVALID")
try:
    rendered = json.loads(sys.argv[2] or "{}")
except json.JSONDecodeError as exc:
    raise SystemExit("SPEC186_EXEC_ENV_INVALID") from exc
allowed_key = re.compile(r"^(?:SPEC186_|SPEC180_|NDNSF_|NDN_)[A-Z0-9_]+$|^(?:CUDA_VISIBLE_DEVICES|PYTHONNOUSERSITE)$")
if not isinstance(rendered, dict) or any(
        not isinstance(key, str) or not allowed_key.fullmatch(key) or
        not isinstance(value, str) or not value
        for key, value in rendered.items()):
    raise SystemExit("SPEC186_EXEC_ENV_INVALID")

# Slurm variables are retained for the job itself, while the workload receives
# only the allow-listed renderer output.  In particular, host LD_LIBRARY_PATH,
# PYTHONPATH and HOME must not leak into a clean candidate image.
child_env = {
    "PATH": "/usr/local/bin:/usr/bin:/bin",
    "HOME": "/tmp/spec186-home",
    "LC_ALL": "C",
}
for name in ("SLURM_JOB_ID", "SLURM_JOB_NODELIST", "CUDA_VISIBLE_DEVICES"):
    value = os.environ.get(name)
    if value:
        child_env[name] = value
child_env.update(rendered)
os.execvpe(argv[0], argv, child_env)
PY
