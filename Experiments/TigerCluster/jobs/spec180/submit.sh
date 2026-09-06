#!/usr/bin/env bash
set -euo pipefail

# The only public Spec180 submission entry point.  The Python boundary renders
# and validates every input before it can invoke sbatch; direct .sbatch calls
# are intentionally unsupported.
ROOT=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)
if [[ $# -ne 3 ]]; then
  echo "usage: $0 {yolo-functional|qwen-functional} PROFILE RUN-RECORD" >&2
  exit 2
fi
case "$1" in
  yolo-functional|qwen-functional) ;;
  *) echo "SPEC180_UNKNOWN_GATE:$1" >&2; exit 2 ;;
esac
[[ -f "$2" ]] || { echo "SPEC180_PROFILE_MISSING:$2" >&2; exit 2; }
[[ -f "$3" ]] || { echo "SPEC180_RUN_RECORD_MISSING:$3" >&2; exit 2; }
exec python3 "$ROOT/scripts/spec180_release.py" submit "$1" "$2" "$3" "$ROOT"
