#!/usr/bin/env bash
set -euo pipefail

# Spec176 deployment probe.  This script does not silently substitute a mock
# flight controller for PX4: without an explicit PX4_SITL_ROOT it records a
# blocked SITL gate and exits non-zero.
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
output_dir="${1:-${repo_root}/results/spec176-uav-sitl-$(date +%Y%m%d_%H%M%S)}"
mkdir -p "${output_dir}"

manifest="${output_dir}/manifest.json"
python3 - "${manifest}" "${repo_root}" <<'PY'
import json
import os
import sys
from pathlib import Path

path = Path(sys.argv[1])
repo_root = Path(sys.argv[2])
payload = {
    "schemaVersion": "spec176-uav-sitl-manifest-v1",
    "missionWindowSeconds": 60,
    "lifecycleChecks": [
        "patrol-and-streams", "incident-success", "incident-failure-keeps-mission",
        "missing-part-compensation", "ambiguous-command-telemetry-reconciliation",
    ],
    "px4SitlRoot": os.environ.get("PX4_SITL_ROOT", ""),
    "scenario": os.environ.get(
        "PX4_SITL_SCENARIO",
        str(repo_root / "NDNSF-UAV-APP/tools/run_uav_px4_sitl_scenario.py"),
    ),
    "flightControllerBackend": os.environ.get("NDNSF_UAV_FLIGHT_CONTROLLER", "udp"),
    "ndnContract": "named producer Data; no IP/host/port/socket application fields",
}
path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
PY

if [[ -z "${PX4_SITL_ROOT:-}" || ! -d "${PX4_SITL_ROOT}" ]]; then
  printf '%s\n' "SPEC176_SITL_BLOCKED reason=PX4_SITL_ROOT-not-configured manifest=${manifest}" >&2
  exit 2
fi

if [[ ! -x "${repo_root}/build/examples/UavGroundStationApp" ||
      ! -x "${repo_root}/build/examples/UavDroneApp" ]]; then
  printf '%s\n' "SPEC176_SITL_BLOCKED reason=UAV-binaries-missing manifest=${manifest}" >&2
  exit 2
fi

scenario="${PX4_SITL_SCENARIO:-${repo_root}/NDNSF-UAV-APP/tools/run_uav_px4_sitl_scenario.py}"
if [[ ! -x "${scenario}" ]]; then
  printf '%s\n' "SPEC176_SITL_BLOCKED reason=scenario-missing path=${scenario} manifest=${manifest}" >&2
  exit 2
fi

set +e
if [[ "${scenario}" == *.py ]]; then
  scenario_command=(python3 "${scenario}")
else
  scenario_command=("${scenario}")
fi
"${scenario_command[@]}" --repo-root "${repo_root}" --output-dir "${output_dir}" \
  >"${output_dir}/sitl.log" 2>&1
rc=$?
set -e
printf '%s\n' "SPEC176_SITL_RESULT returncode=${rc} manifest=${manifest} log=${output_dir}/sitl.log"
exit "${rc}"
