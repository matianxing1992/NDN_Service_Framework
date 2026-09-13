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
python3 - "$SPEC186_EXEC_ARGV" <<'PY'
import json
import os
import sys

argv = json.loads(sys.argv[1])
if not isinstance(argv, list) or not argv or any(not isinstance(item, str) or not item for item in argv):
    raise SystemExit("SPEC186_EXEC_ARGV_INVALID")
os.execv(argv[0], argv)
PY
