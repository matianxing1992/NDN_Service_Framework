#!/usr/bin/env bash
set -euo pipefail

: "${SPEC175_SIF:?exact candidate SIF is required}"
: "${SPEC175_SIF_SHA256:?candidate SIF digest is required}"
: "${SPEC175_BUNDLE:?bundle directory is required}"
: "${SPEC175_OUTPUT:?output directory is required}"
: "${SPEC175_GATE:?gate name is required}"
if [[ "$SPEC175_GATE" == performance ]]; then
  : "${SPEC175_G5_MANIFEST:?G7 requires the passing G5 prerequisite manifest}"
  : "${SPEC175_G6_MANIFEST:?G7 requires the passing G6 prerequisite manifest}"
  : "${SPEC175_G6C_MANIFEST:?G7 requires the passing G6C prerequisite manifest}"
fi

# Model weights/tokenizer remain outside the immutable SIF.  Functional gates
# must opt into one content-addressed, read-only model root; the control gate
# has no model and therefore does not require this bind.
if [[ "$SPEC175_GATE" != control ]]; then
  : "${SPEC175_MODEL_ROOT:?external model root is required for this gate}"
  [[ -d "$SPEC175_MODEL_ROOT" ]] || {
    echo "SPEC175_MODEL_ROOT_MISSING" >&2
    exit 4
  }
fi

actual=$(sha256sum "$SPEC175_SIF" | awk '{print $1}')
[[ "$actual" == "${SPEC175_SIF_SHA256#sha256:}" ]] || {
  echo "SPEC175_SIF_DIGEST_MISMATCH" >&2
  exit 4
}
[[ -d "$SPEC175_BUNDLE" ]] || { echo "SPEC175_BUNDLE_MISSING" >&2; exit 4; }
mkdir -p "$SPEC175_OUTPUT"
if [[ "$SPEC175_GATE" != control && -n "${SPEC175_STAGE_MANIFEST_OVERRIDE:-}" ]]; then
  [[ -f "$SPEC175_STAGE_MANIFEST_OVERRIDE" ]] || {
    echo "SPEC175_STAGE_MANIFEST_OVERRIDE_MISSING" >&2
    exit 4
  }
fi

manifest="$SPEC175_BUNDLE/spec175-functional-manifest.json"
schema=""
group_specs=()
if [[ "$SPEC175_GATE" != control && -f "$manifest" ]]; then
  schema=$(python3 - "$manifest" <<'PY'
import json
import sys
print(json.load(open(sys.argv[1], encoding="utf-8")).get("schemaVersion", ""))
PY
)
fi
if [[ "$schema" == ndnsf-di-spec175-functional-bundle-v2 ]]; then
  mapfile -t group_specs < <(python3 - "$manifest" <<'PY'
import json
import pathlib
import re
import sys

document = json.load(open(sys.argv[1], encoding="utf-8"))
groups = document.get("processGroups")
if not isinstance(groups, list) or not groups:
    raise SystemExit("SPEC175_PROCESS_GROUPS_MISSING")
for group in groups:
    group_id = str(group.get("id", ""))
    record = group.get("userArgs")
    path = record.get("path") if isinstance(record, dict) else ""
    if not re.fullmatch(r"[A-Za-z0-9._-]+", group_id):
        raise SystemExit("SPEC175_PROCESS_GROUP_ID_INVALID")
    candidate = pathlib.PurePosixPath(str(path))
    if candidate.is_absolute() or ".." in candidate.parts or not str(path):
        raise SystemExit("SPEC175_PROCESS_GROUP_USER_ARGS_INVALID")
    print(f"{group_id}\t{candidate}")
PY
  )
  [[ ${#group_specs[@]} -gt 0 ]] || {
    echo "SPEC175_PROCESS_GROUPS_MISSING" >&2
    exit 4
  }
else
  group_specs=($'legacy\tuser.args')
fi

# Provider children inherit the bundle cwd so relative model/tokenizer paths
# cannot resolve against the scheduler's submit directory.
apptainer_args=(exec --cleanenv)
if [[ "$SPEC175_GATE" != control ]]; then
  # A Slurm GPU allocation alone does not expose the device or host driver
  # libraries inside Apptainer.  Every model gate is CUDA-required and must
  # enter the exact SIF through --nv; the CPU control remains GPU-independent.
  apptainer_args+=(--nv)
  apptainer_args+=(
    --env "NDNSF_SVS_PERIODIC_SYNC_MS=${SPEC175_SVS_PERIODIC_SYNC_MS:-1000}"
    --env "NDNSF_DI_TOKENIZER_JSON=/model/qwen-onnx-tokenizer/tokenizer.json"
  )
fi
run_group() {
  local group_id=$1
  local user_args_relative=$2
  local group_output=$3
  local user_args_container="/bundle/$user_args_relative"
  local binds="$SPEC175_BUNDLE:/bundle:ro,$group_output:/evidence"
  if [[ "$SPEC175_GATE" != control ]]; then
    binds="$binds,$SPEC175_MODEL_ROOT:/model:ro"
    if [[ -n "${SPEC175_STAGE_MANIFEST_OVERRIDE:-}" ]]; then
      binds="$binds,$SPEC175_STAGE_MANIFEST_OVERRIDE:/model/qwen36-stage-manifest.json:ro"
    fi
  fi
  mkdir -p "$group_output"
  local monitor_pid=""
  local monitor_status=0
  if [[ "$SPEC175_GATE" == performance ]]; then
    [[ -f "$SPEC175_BUNDLE/collect-resources.py" ]] || {
      echo "SPEC175_G7_RESOURCE_COLLECTOR_MISSING" >&2
      return 4
    }
    python3 "$SPEC175_BUNDLE/collect-resources.py" \
      --output "$group_output/resource-samples.jsonl" \
      --parent-pid "$$" --interval-ms 500 &
    monitor_pid=$!
  fi
  set +e
  apptainer "${apptainer_args[@]}" \
    --env "SLURM_JOB_ID=${SLURM_JOB_ID:-spec175-local}" \
    --env "SPEC175_GATE=${SPEC175_GATE}" \
    --env "SPEC175_PROCESS_GROUP_ID=${group_id}" \
    --env "SPEC175_USER_ARGS=${user_args_container}" \
    --bind "$binds" \
    "$SPEC175_SIF" /bin/bash -lc '
    cd /bundle
    test -x /opt/ndnsf/bin/run-ndnsf-qwen.sh || {
      echo SPEC175_SIF_CONTROL_ENTRYPOINT_MISSING >&2
      exit 4
    }
    test -f /bundle/nfd.conf || { echo SPEC175_BUNDLE_NFD_CONFIG_MISSING >&2; exit 4; }
    test -f /bundle/controller.args || { echo SPEC175_BUNDLE_CONTROLLER_ARGS_MISSING >&2; exit 4; }
    test -d /bundle/providers || { echo SPEC175_BUNDLE_PROVIDER_ARGS_MISSING >&2; exit 4; }
    test -f "$SPEC175_USER_ARGS" || { echo SPEC175_BUNDLE_USER_ARGS_MISSING >&2; exit 4; }
    set +e
    /opt/ndnsf/bin/run-ndnsf-qwen.sh \
      --scratch /evidence/runtime \
      --evidence /evidence \
      --nfd-config /bundle/nfd.conf \
      --controller-args /bundle/controller.args \
      --provider-args-dir /bundle/providers \
      --user-args "$SPEC175_USER_ARGS"
    status=$?
    set -e
    printf "{\"schemaVersion\":\"spec175-sif-control-v1\",\"gate\":\"%s\",\"exitCode\":%d,\"status\":\"%s\"}\n" \
      "$SPEC175_GATE" "$status" "$([[ $status -eq 0 ]] && echo PASS || echo FAIL)" \
      >/evidence/spec175-process-terminal.json
    exit "$status"
  '
  local status=$?
  set -e
  if [[ -n "$monitor_pid" ]]; then
    kill -TERM "$monitor_pid" 2>/dev/null || true
    set +e
    wait "$monitor_pid"
    monitor_status=$?
    set -e
    if [[ "$monitor_status" -ne 0 ]]; then
      echo "SPEC175_G7_RESOURCE_COLLECTOR_FAILED status=$monitor_status" >&2
      status=4
    fi
  fi
  if [[ "$status" -eq 0 && "$SPEC175_GATE" == conversation-residency ]]; then
    if [[ ! -f "$SPEC175_BUNDLE/analyze-conversation-residency.py" ]]; then
      echo "SPEC175_G6C_ANALYZER_MISSING" >&2
      status=4
    else
      set +e
      python3 "$SPEC175_BUNDLE/analyze-conversation-residency.py" \
        --evidence-root "$group_output" \
        --bundle "$SPEC175_BUNDLE" \
        --output "$group_output/g6c-analysis.json"
      status=$?
      set -e
    fi
  fi
  python3 - "$group_output/runner-terminal.json" "$group_id" "$status" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
group_id = sys.argv[2]
status = int(sys.argv[3])
path.write_text(json.dumps({
    "schemaVersion": "ndnsf-di-spec175-process-terminal-v1",
    "processGroupId": group_id,
    "exitCode": status,
    "status": "PASS" if status == 0 else "FAIL",
}, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")
PY
  return "$status"
}

overall_status=0
completed_groups=0
terminal_paths=()
for spec in "${group_specs[@]}"; do
  IFS=$'\t' read -r group_id user_args_relative <<<"$spec"
  if [[ "$schema" == ndnsf-di-spec175-functional-bundle-v2 ]]; then
    group_output="$SPEC175_OUTPUT/process-groups/$group_id"
  else
    group_output="$SPEC175_OUTPUT"
  fi
  if run_group "$group_id" "$user_args_relative" "$group_output"; then
    completed_groups=$((completed_groups + 1))
  else
    overall_status=$?
    terminal_paths+=("$group_output/runner-terminal.json")
    break
  fi
  terminal_paths+=("$group_output/runner-terminal.json")
done

if [[ "$overall_status" -eq 0 && "$SPEC175_GATE" == performance ]]; then
  if [[ ! -f "$SPEC175_BUNDLE/analyze-performance.py" ]]; then
    echo "SPEC175_G7_ANALYZER_MISSING" >&2
    overall_status=4
  else
    set +e
    python3 "$SPEC175_BUNDLE/analyze-performance.py" \
      --output-root "$SPEC175_OUTPUT" \
      --bundle "$SPEC175_BUNDLE" \
      --g5-manifest "$SPEC175_G5_MANIFEST" \
      --g6-manifest "$SPEC175_G6_MANIFEST" \
      --g6c-manifest "$SPEC175_G6C_MANIFEST" \
      --output "$SPEC175_OUTPUT/g7-analysis.json"
    overall_status=$?
    set -e
  fi
fi

python3 - "$SPEC175_OUTPUT/spec175-gate-terminal.json" "$SPEC175_GATE" \
  "$overall_status" "${#group_specs[@]}" "$completed_groups" \
  "${terminal_paths[@]}" <<'PY'
import json
import pathlib
import sys

output = pathlib.Path(sys.argv[1])
gate = sys.argv[2]
status = int(sys.argv[3])
expected = int(sys.argv[4])
completed = int(sys.argv[5])
groups = []
for value in sys.argv[6:]:
    path = pathlib.Path(value)
    if path.is_file():
        groups.append(json.loads(path.read_text(encoding="utf-8")))
document = {
    "schemaVersion": "ndnsf-di-spec175-gate-terminal-v2",
    "gate": gate,
    "exitCode": status,
    "status": "PASS" if status == 0 and completed == expected else "FAIL",
    "expectedProcessGroups": expected,
    "completedProcessGroups": completed,
    "processGroups": groups,
}
output.write_text(
    json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n",
    encoding="utf-8",
)
PY
exit "$overall_status"
