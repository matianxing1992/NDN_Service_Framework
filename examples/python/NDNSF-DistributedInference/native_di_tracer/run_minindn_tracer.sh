#!/usr/bin/env bash
set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
example_dir="${repo_root}/examples/python/NDNSF-DistributedInference/native_di_tracer"
out_dir="${repo_root}/results/native_di_tracer/latest"
require_minindn=0
assignment="default"
original_command="$0 $*"

usage() {
  echo "usage: $0 [--out DIR] [--assignment default|alternate] [--require-minindn]"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out)
      if [[ $# -lt 2 ]]; then
        usage >&2
        exit 2
      fi
      out_dir="$2"
      shift 2
      ;;
    --assignment)
      if [[ $# -lt 2 ]]; then
        usage >&2
        exit 2
      fi
      assignment="$2"
      shift 2
      ;;
    --require-minindn)
      require_minindn=1
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      usage >&2
      exit 2
      ;;
  esac
done

case "${assignment}" in
  default)
    provider_prefix="/NDNSF-DI/Tracer/provider"
    ;;
  alternate)
    provider_prefix="/NDNSF-DI/Tracer/alt-provider"
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac

policy_dir="${out_dir}/policy-bundle"
logs_dir="${out_dir}/logs"
summary="${out_dir}/summary.txt"
summary_json="${out_dir}/summary.json"
timing_csv="${out_dir}/timing.csv"
assignment_csv="${out_dir}/assignment.csv"

rm -rf "${out_dir}"
mkdir -p "${policy_dir}" "${logs_dir}"

cd "${repo_root}"
export PYTHONPATH="${repo_root}/NDNSF-DistributedInference:${repo_root}/pythonWrapper:${PYTHONPATH:-}"
export LD_LIBRARY_PATH="${repo_root}/build:${LD_LIBRARY_PATH:-}"

status=0
failure_reason=""

run_step() {
  local name="$1"
  shift
  echo "RUN ${name}: $*" | tee "${logs_dir}/${name}.log"
  "$@" >>"${logs_dir}/${name}.log" 2>&1
}

minindn_status="$(python3 - <<'PY'
import os
try:
    import minindn  # noqa: F401
except Exception as exc:
    print(f"unavailable:{exc}")
else:
    if os.geteuid() == 0:
        print("available-root")
    else:
        print("available-non-root")
PY
)"

{
  run_step plan-tracer \
    python3 "${example_dir}/plan_tracer.py" \
      --out "${policy_dir}" \
      --summary-json "${out_dir}/policy-summary.json" &&
  run_step schema-smoke \
    "${repo_root}/build/examples/di-native-plan-schema-smoke" \
      "${policy_dir}/native-execution-plan.json" \
      /Inference/NativeTracer yolo-onnx onnx yolo-detect-auto &&
  run_step assignment-validate \
    python3 - "${policy_dir}/native-execution-plan.json" "${assignment_csv}" "${assignment}" "${provider_prefix}" <<'PY' &&
import csv
import json
import sys

plan_path, assignment_csv, assignment, provider_prefix = sys.argv[1:5]
plan = json.loads(open(plan_path, encoding="utf-8").read())
service = next(item for item in plan["services"] if item["service"] == "/Inference/NativeTracer")
roles = service["roles"]
expected_roles = ["/Backbone", "/Head/Shard/0", "/Head/Shard/1", "/Merge"]
if sorted(roles) != sorted(expected_roles):
    raise SystemExit(f"unexpected roles in native plan: {roles}")
suffix_by_role = {
    "/Backbone": "backbone",
    "/Head/Shard/0": "head0",
    "/Head/Shard/1": "head1",
    "/Merge": "merge",
}
providers = {role: f"{provider_prefix}/{suffix_by_role[role]}" for role in roles}
if sorted(providers) != sorted(expected_roles):
    raise SystemExit("assignment is missing a required role")
with open(assignment_csv, "w", newline="", encoding="utf-8") as output:
    writer = csv.DictWriter(output, fieldnames=["assignment", "role", "provider"])
    writer.writeheader()
    for role in expected_roles:
        writer.writerow({"assignment": assignment, "role": role, "provider": providers[role]})
print(f"NDNSF_DI_ASSIGNMENT_OK assignment={assignment} roles={len(providers)}")
PY
  run_step manifest-smoke \
    "${repo_root}/build/examples/di-native-plan-manifest-smoke" \
      "${policy_dir}/native-execution-plan.json" \
      "${policy_dir}/service-manifest.json" \
      /Inference/NativeTracer \
      --timing-csv "${timing_csv}" \
      --assignment "${assignment}" &&
  run_step evidence-validate \
    python3 - "${timing_csv}" "${assignment_csv}" <<'PY' &&
import csv
import sys

timing_csv, assignment_csv = sys.argv[1:3]
required_columns = [
    "sessionId", "provider", "role", "inputBytes", "outputBytes",
    "prefetchMs", "executeMs", "publishMs", "endToEndMs", "status",
]
with open(assignment_csv, newline="", encoding="utf-8") as input_file:
    expected = {row["role"]: row["provider"] for row in csv.DictReader(input_file)}
with open(timing_csv, newline="", encoding="utf-8") as input_file:
    reader = csv.DictReader(input_file)
    if reader.fieldnames != required_columns:
        raise SystemExit(f"unexpected timing columns: {reader.fieldnames}")
    rows = list(reader)
if {row["role"] for row in rows} != set(expected):
    raise SystemExit("timing rows do not match assignment roles")
for row in rows:
    if row["provider"] != expected[row["role"]]:
        raise SystemExit(f"provider mismatch for {row['role']}: {row['provider']} != {expected[row['role']]}")
    if row["status"] != "ok":
        raise SystemExit(f"non-ok provider status: {row}")
    int(row["inputBytes"])
    int(row["outputBytes"])
    for column in ("prefetchMs", "executeMs", "publishMs", "endToEndMs"):
        float(row[column])
print(f"NDNSF_DI_EVIDENCE_OK rows={len(rows)}")
PY
  run_step provider-session-smoke \
    "${repo_root}/build/examples/di-native-provider-session-smoke"
} || {
  status=$?
  failure_reason="native tracer local evidence failed; see ${logs_dir}"
}

if [[ "${status}" -eq 0 && "${require_minindn}" -eq 1 && "${minindn_status}" != "available-root" ]]; then
  status=1
  failure_reason="MiniNDN was required but status is ${minindn_status}"
fi

if [[ ! -s "${timing_csv}" && "${status}" -eq 0 ]]; then
  status=1
  failure_reason="timing.csv was not created"
fi

if [[ "${status}" -eq 0 ]]; then
  marker="SUCCESS"
  touch "${out_dir}/SUCCESS"
else
  marker="FAILURE"
  touch "${out_dir}/FAILURE"
fi

git_commit="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
if [[ "${status}" -eq 0 && "${minindn_status}" == "available-root" ]]; then
  minindn_run="ready-to-enable; this tracer currently validates native DI evidence before launching a full topology"
else
  minindn_run="not-started; use sudo or --require-minindn for hard MiniNDN gating"
fi

python3 - "${summary_json}" \
  "${marker}" \
  "${git_commit}" \
  "${original_command}" \
  "${out_dir}" \
  "${policy_dir}" \
  "${timing_csv}" \
  "${assignment_csv}" \
  "${logs_dir}" \
  "${minindn_status}" \
  "${minindn_run}" \
  "${assignment}" \
  "${failure_reason}" <<'PY'
import json
import pathlib
import sys

(
    summary_json,
    marker,
    git_commit,
    original_command,
    out_dir,
    policy_dir,
    timing_csv,
    assignment_csv,
    logs_dir,
    minindn_status,
    minindn_run,
    assignment,
    failure_reason,
) = sys.argv[1:14]

summary = {
    "status": marker,
    "gitCommit": git_commit,
    "command": original_command,
    "resultDir": out_dir,
    "policyBundle": policy_dir,
    "nativePlan": f"{policy_dir}/native-execution-plan.json",
    "serviceManifest": f"{policy_dir}/service-manifest.json",
    "timingCsv": timing_csv,
    "assignmentCsv": assignment_csv,
    "logs": logs_dir,
    "miniNDNStatus": minindn_status,
    "miniNDNRun": minindn_run,
    "assignment": assignment,
    "llmPlannerGate": "blocked-until-native-tracer-evidence-accepted",
    "failureReason": failure_reason,
}
pathlib.Path(summary_json).write_text(
    json.dumps(summary, indent=2, sort_keys=True) + "\n",
    encoding="utf-8")
PY

{
  echo "NDNSF Native DI Tracer Evidence"
  echo "status=${marker}"
  echo "gitCommit=${git_commit}"
  echo "command=${original_command}"
  echo "resultDir=${out_dir}"
  echo "policyBundle=${policy_dir}"
  echo "nativePlan=${policy_dir}/native-execution-plan.json"
  echo "serviceManifest=${policy_dir}/service-manifest.json"
  echo "timingCsv=${timing_csv}"
  echo "assignmentCsv=${assignment_csv}"
  echo "summaryJson=${summary_json}"
  echo "logs=${logs_dir}"
  echo "miniNDNStatus=${minindn_status}"
  echo "miniNDNRun=${minindn_run}"
  echo "assignment=${assignment}"
  echo "llmPlannerGate=blocked-until-native-tracer-evidence-accepted"
  if [[ -n "${failure_reason}" ]]; then
    echo "failureReason=${failure_reason}"
  fi
} >"${summary}"

cat "${summary}"
exit "${status}"
