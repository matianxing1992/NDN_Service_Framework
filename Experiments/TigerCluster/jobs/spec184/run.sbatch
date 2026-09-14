#!/usr/bin/env bash
# Spec186 scheduler payload.  The submit.py boundary supplies the effective
# candidate and run identity; this file never discovers host paths implicitly.
set -euo pipefail
umask 077
: "${SPEC186_RUN_ID:?SPEC186_RUN_ID is required}"
: "${SPEC186_EFFECTIVE_CONFIG:?SPEC186_EFFECTIVE_CONFIG is required}"
echo "$SPEC186_EFFECTIVE_CONFIG"
if [[ "${SPEC186_EXECUTE:-0}" != "1" ]]; then
  echo "SPEC186_SCHEDULER_PAYLOAD_READY run=${SPEC186_RUN_ID}"
  exit 0
fi
: "${SPEC186_EXEC_ARGV:?SPEC186_EXEC_ARGV is required}"
python3 - "$SPEC186_EFFECTIVE_CONFIG" "$SPEC186_EXEC_ARGV" "${SPEC186_EXEC_ENV:-}" <<'PY'
import json
import os
from pathlib import Path
import re
import secrets
import sys

try:
    config = json.loads(sys.argv[1])
    if (not isinstance(config, dict) or
            config.get("schemaVersion") != "spec186-effective-config-v1" or
            config.get("runId") != os.environ.get("SPEC186_RUN_ID") or
            not isinstance(config.get("runId"), str) or
            re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}", config["runId"]) is None or
            not isinstance(config.get("candidateDigest"), str) or
            not re.fullmatch(r"[0-9a-f]{64}", config["candidateDigest"]) or
            not isinstance(config.get("runRoot"), str) or
            "\x00" in config["runRoot"]):
        raise ValueError("identity")
    run_root = Path(config["runRoot"])
except (KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
    raise SystemExit("SPEC186_EFFECTIVE_CONFIG_INVALID") from exc
if (not run_root.is_absolute() or ".." in run_root.parts or
        run_root == run_root.parent):
    raise SystemExit("SPEC186_RUN_ROOT_INVALID")

try:
    argv = json.loads(sys.argv[2])
except (TypeError, json.JSONDecodeError) as exc:
    raise SystemExit("SPEC186_EXEC_ARGV_INVALID") from exc
if (not isinstance(argv, list) or not argv or
        any(not isinstance(item, str) or not item or "\x00" in item for item in argv) or
        not os.path.isabs(argv[0])):
    raise SystemExit("SPEC186_EXEC_ARGV_INVALID")
execution = config.get("execution", {})
container_launcher = execution.get("containerLauncher") if isinstance(execution, dict) else None
if container_launcher is not None:
    if (not isinstance(container_launcher, list) or not container_launcher or
            any(not isinstance(item, str) or not item or "\x00" in item
                for item in container_launcher) or
            not os.path.isabs(container_launcher[0]) or
            Path(container_launcher[0]).name != "apptainer"):
        raise SystemExit("SPEC186_CONTAINER_LAUNCHER_INVALID")
    argv = container_launcher + argv
try:
    rendered = json.loads(sys.argv[3] or "{}")
except (TypeError, json.JSONDecodeError) as exc:
    raise SystemExit("SPEC186_EXEC_ENV_INVALID") from exc
allowed_key = re.compile(r"^(?:SPEC186_|SPEC180_|NDNSF_|NDN_)[A-Z0-9_]+$|^(?:CUDA_VISIBLE_DEVICES|PYTHONNOUSERSITE)$")
if not isinstance(rendered, dict) or any(
        not isinstance(key, str) or not allowed_key.fullmatch(key) or
        not isinstance(value, str) or not value or "\x00" in value
        for key, value in rendered.items()):
    raise SystemExit("SPEC186_EXEC_ENV_INVALID")
for identity_key, expected in (("SPEC186_RUN_ID", config["runId"]),
                               ("SPEC186_CANDIDATE_DIGEST", config["candidateDigest"])):
    if identity_key in rendered and rendered[identity_key] != expected:
        raise SystemExit("SPEC186_EXEC_ENV_IDENTITY_MISMATCH")

try:
    run_root.mkdir(parents=True, exist_ok=False)
    for child in ("evidence", "state", "security", "home"):
        (run_root / child).mkdir(mode=0o700)
    envelope_key = run_root / "security" / "request-envelope.key"
    envelope_key.write_bytes(secrets.token_bytes(32))
    envelope_key.chmod(0o600)
    os.chdir(run_root)
except (OSError, ValueError) as exc:
    raise SystemExit("SPEC186_RUN_ROOT_CREATE_FAILED") from exc

# Slurm variables are retained for the job itself, while the workload receives
# only the allow-listed renderer output.  In particular, host LD_LIBRARY_PATH,
# PYTHONPATH and HOME must not leak into a clean candidate image.
child_env = {
    "PATH": "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin",
    # Keep HOME inside the run-owned root.  A fixed /tmp path would let
    # concurrent jobs share PIB/TPM/client.conf state and would make a reused
    # allocation depend on leftovers from another run.
    "HOME": str(run_root / "home"),
    "LC_ALL": "C",
}
for name in ("SLURM_JOB_ID", "SLURM_JOB_NODELIST", "CUDA_VISIBLE_DEVICES"):
    value = os.environ.get(name)
    if value:
        child_env[name] = value
child_env.update(rendered)
try:
    os.execvpe(argv[0], argv, child_env)
except (OSError, ValueError) as exc:
    raise SystemExit("SPEC186_EXEC_FAILED:" + type(exc).__name__) from exc
PY
