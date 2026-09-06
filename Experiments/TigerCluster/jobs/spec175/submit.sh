#!/usr/bin/env bash
set -euo pipefail

# The only public Tiger submission entry point. All workload/configuration
# values come from the checked-in proven profile and one run record.
ROOT=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)
if [[ $# -ne 3 ]]; then
  echo "usage: $0 {control|stage-readiness|multi-provider|conversation-residency|performance} proven-tiger-profile.json run-record.json" >&2
  exit 2
fi
case "$1" in
  control|stage-readiness|multi-provider|conversation-residency|performance) ;;
  *) echo "SPEC175_UNKNOWN_GATE:$1" >&2; exit 2 ;;
esac
[[ -f "$2" ]] || { echo "SPEC175_PROFILE_MISSING:$2" >&2; exit 2; }
[[ -f "$3" ]] || { echo "SPEC175_RUN_RECORD_MISSING:$3" >&2; exit 2; }
exec python3 "$ROOT/packaging/ndnsf-di-container/jobs/spec175/submit_profile.py" \
  submit "$1" "$2" "$3" "$ROOT"
