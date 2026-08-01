#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
output_dir="${SPEC164_OUTPUT_DIR:-$repo_root/results/spec164-file-producer-registration-$(date -u +%Y%m%dT%H%M%SZ)}"

cd "$repo_root"
sudo -n env \
  PYTHONPATH="$repo_root/pythonWrapper:$repo_root/NDNSF-DistributedRepo/pythonWrapper" \
  LD_LIBRARY_PATH="$repo_root/build:$repo_root/build/lib:$repo_root/build/NDNSF-DistributedRepo${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  python3 Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py \
  --performance-subject signed-manifest \
  --payload-size 1048576 \
  --replicas 1 \
  --concurrency 4 \
  --timeout-seconds 30 \
  --measurement-window-seconds 60 \
  --output-dir "$output_dir"

python3 - "$output_dir/signed-manifest/summary.json" <<'PY'
import json
from pathlib import Path
import sys

summary = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
assert summary["verdict"] == "PASS", summary
assert summary["committedReplicas"] == 1, summary
assert summary["coldDestinationVisible"] is True, summary
assert len(summary["workers"]) == 4, summary
assert len(summary["coldWorkers"]) == 4, summary
assert all(worker["status"] == "SUCCESS" for worker in summary["workers"]), summary
assert all(worker["status"] == "SUCCESS" for worker in summary["coldWorkers"]), summary
print("SPEC164_FILE_PRODUCER_REGISTRATION_REGRESSION_OK", sys.argv[1])
PY
