#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=3
#SBATCH --ntasks=3
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=32G
#SBATCH --gres=gpu:rtx_5000:1
#SBATCH --time=00:20:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec160/qwen-live/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec160/qwen-live/slurm-%j.err

set -Eeuo pipefail
umask 077
: "${SPEC160_LIVE_RUN_ID:?}"
: "${SPEC160_LIVE_SUBMISSION_ID:?}"
: "${SPEC160_LIVE_SOURCE_ROOT:?}"
: "${SPEC160_LIVE_SOURCE_SHA256:?}"
: "${SPEC160_SIF:?}"
: "${SPEC160_SIF_SHA256:?}"
: "${SPEC160_ARTIFACT_DIR:?}"
: "${SPEC160_REQUEST_ID:?}"
: "${SPEC160_LIVE_REPLACES_JOB:?}"

readonly evidence_parent='/project/tma1/ndnsf-di/evidence/spec160/qwen-live'
readonly partial="${evidence_parent}/.${SPEC160_LIVE_SUBMISSION_ID}.partial"
readonly final="${evidence_parent}/${SPEC160_LIVE_SUBMISSION_ID}"
test ! -e "$partial" && test ! -e "$final"
mkdir -p "$partial/source"
cp -a "$SPEC160_LIVE_SOURCE_ROOT"/llm_pipeline "$partial/source/"
cp "$SPEC160_LIVE_SOURCE_ROOT"/nfd.conf.in \
   "$SPEC160_LIVE_SOURCE_ROOT"/ndnsf-qwen-rank.sh \
   "$SPEC160_LIVE_SOURCE_ROOT"/ndnsf-qwen-rank-inner.sh \
   "$SPEC160_LIVE_SOURCE_ROOT"/ndnsf-qwen-multinode.sbatch \
   "$partial/source/"
chmod 700 "$partial/source/ndnsf-qwen-rank.sh" "$partial/source/ndnsf-qwen-rank-inner.sh"
printf '%s  source-bundle\n' "$SPEC160_LIVE_SOURCE_SHA256" > "$partial/source-bundle.sha256"
printf '%s  %s\n' "$SPEC160_SIF_SHA256" "$SPEC160_SIF" | sha256sum -c - > "$partial/sif-sha256.log"
printf '%s\n' "$SPEC160_ARTIFACT_DIR" > "$partial/artifact-dir.txt"
cp "$SPEC160_ARTIFACT_DIR/policy.yaml" "$partial/policy.yaml"
cp "$SPEC160_ARTIFACT_DIR/qwen-pipeline-runtime.json" \
   "$partial/qwen-pipeline-runtime.json"
{
  echo "runtimeContract=clean-sif"
  echo "noMountedNativeLibs=true"
  echo "noMountedPythonBindings=true"
  echo "noMountedVendorSite=true"
} > "$partial/runtime-contract.txt"

terminal_state=FAIL
finish()
{
  rc=$?
  trap - EXIT INT TERM
  rm -f -- "$partial/bootstrap-tokens.txt"
  python3 - "$partial/result.json" "$terminal_state" "$rc" <<'PY' || true
import json,os,sys
path,state,rc=sys.argv[1:]
with open(path,"w",encoding="utf-8") as f:
  json.dump({"schemaVersion":"spec160-qwen-live-v1","state":state,
             "exitCode":int(rc),"jobId":os.environ["SLURM_JOB_ID"],
             "runId":os.environ["SPEC160_LIVE_RUN_ID"],
             "submissionId":os.environ["SPEC160_LIVE_SUBMISSION_ID"],
             "requestId":os.environ["SPEC160_REQUEST_ID"],
             "replacesJobId":os.environ["SPEC160_LIVE_REPLACES_JOB"],
             "sifSha256":os.environ["SPEC160_SIF_SHA256"],
             "sourceSha256":os.environ["SPEC160_LIVE_SOURCE_SHA256"]},
            f,indent=2,sort_keys=True)
  f.write("\n")
PY
  (
    cd "$partial"
    find . -type f ! -name checksums.sha256 -print0 |
      sort -z |
      xargs -0 sha256sum
  ) > "$partial/checksums.sha256" 2>/dev/null || true
  if test "$terminal_state" = PASS && test "$rc" -eq 0; then
    mv "$partial" "$final"
  fi
  exit "$rc"
}
trap finish EXIT INT TERM

comment=$(scontrol show job "$SLURM_JOB_ID" -o | tr ' ' '\n' | sed -n 's/^Comment=//p')
test "$comment" = "spec160:${SPEC160_LIVE_SUBMISSION_ID}"
mapfile -t nodes < <(scontrol show hostnames "$SLURM_JOB_NODELIST")
printf '%s\n' "${nodes[@]}" > "$partial/nodes.txt"
test "${#nodes[@]}" -eq 3
port=$((20000 + SLURM_JOB_ID % 20000))
printf '%s\n' "$port" > "$partial/port.txt"
export SPEC160_SHARED="$partial"
export SPEC160_PORT="$port"
export SPEC160_POLICY="$SPEC160_ARTIFACT_DIR/policy.yaml"
export SPEC160_RUNTIME_SUMMARY="$SPEC160_ARTIFACT_DIR/qwen-pipeline-runtime.json"
srun --kill-on-bad-exit=0 --label \
  --export=ALL,SPEC160_SHARED="$partial",SPEC160_SIF="$SPEC160_SIF",SPEC160_PORT="$port",SPEC160_ARTIFACT_DIR="$SPEC160_ARTIFACT_DIR",SPEC160_POLICY="$SPEC160_POLICY",SPEC160_RUNTIME_SUMMARY="$SPEC160_RUNTIME_SUMMARY",SPEC160_REQUEST_ID="$SPEC160_REQUEST_ID" \
  "$partial/source/ndnsf-qwen-rank.sh" \
  > "$partial/rank-step.log" 2> "$partial/rank-step.err"
test -f "$partial/rank-complete-0"
test -f "$partial/rank-complete-1"
test -f "$partial/rank-complete-2"
grep -q 'LLM_PIPELINE_USER_RESPONSE' "$partial/node-0/user.log"
grep -q 'LLM_PIPELINE_USER_SUMMARY' "$partial/node-0/user.log"
if grep -RniE 'double free|invalid pointer|munmap_chunk|Aborted|core dumped|CPUExecutionProvider|QWEN_STAGE_CPU_FALLBACK_FORBIDDEN|QWEN_STAGE_CUDA_UNAVAILABLE|reportOperationStatus.*incompatible function arguments' \
  "$partial"/node-* "$partial"/rank-step.log "$partial"/rank-step.err; then
  echo SPEC160_LIVE_FORBIDDEN_RUNTIME_MARKER >&2
  exit 8
fi
test ! -e "$partial/bootstrap-tokens.txt"
test -z "$(find "$partial" -type f \( -name '*.privkey' -o -name 'bootstrap-tokens.txt' \) -print -quit)"
python3 - "$partial/analysis.json" "$partial" "$SPEC160_RUNTIME_SUMMARY" \
  "$SPEC160_REQUEST_ID" <<'PY'
import csv,json,re,sys
from pathlib import Path
out=Path(sys.argv[1]); root=Path(sys.argv[2])
runtime=json.loads(Path(sys.argv[3]).read_text(encoding="utf-8"))
request_id=sys.argv[4]

def require(condition, message):
    if not condition:
        raise SystemExit(message)

def marker_fields(log, marker):
    rows=[]
    for line in log.splitlines():
        if marker not in line:
            continue
        tail=line.split(marker,1)[1].strip()
        rows.append(dict(item.split("=",1) for item in tail.split()
                         if "=" in item))
    return rows

nodes=[line.strip() for line in (root/"nodes.txt").read_text().splitlines()
       if line.strip()]
require(len(nodes) == 3 and len(set(nodes)) == 3,
        f"expected three distinct nodes, got {nodes}")
gpus=[]
for rank in range(3):
    row=next(csv.reader([(root/f"node-{rank}/gpu.csv").read_text().splitlines()[0]]))
    row=[item.strip() for item in row]
    require(len(row) >= 2 and row[0].startswith("GPU-"),
            f"rank {rank} invalid GPU identity: {row}")
    gpus.append({"rank":rank,"uuid":row[0],"name":row[1]})
require(len({row["uuid"] for row in gpus}) == 3,
        f"expected three distinct GPUs, got {gpus}")

providers=[]
timings=[]
expected_roles=[f"/LLM/Pipeline/Stage/{rank}" for rank in range(3)]
for rank in range(3):
    log=(root/f"node-{rank}/provider-{rank}.log").read_text(encoding="utf-8", errors="replace")
    require("LLM_PIPELINE_PROVIDER_READY" in log, f"provider {rank} not ready")
    rows=marker_fields(log, "LLM_PIPELINE_QWEN_STAGE_TIMING")
    require(len(rows) == 1, f"provider {rank} expected one stage timing, got {len(rows)}")
    timing=rows[0]
    require(timing.get("role") == expected_roles[rank],
            f"provider {rank} role mismatch: {timing.get('role')}")
    require(int(timing.get("stage","-1")) == rank,
            f"provider {rank} stage mismatch: {timing.get('stage')}")
    require(timing.get("requestId") == f"{request_id}-0",
            f"provider {rank} request ID mismatch: {timing.get('requestId')}")
    require(timing.get("device","").startswith("cuda"),
            f"provider {rank} did not execute on CUDA: {timing.get('device')}")
    require(timing.get("cpuFallback","").lower() in {"0","false"},
            f"provider {rank} used CPU fallback: {timing.get('cpuFallback')}")
    for key in ("input_sha256","output_sha256"):
        require(re.fullmatch(r"[0-9a-f]{64}", timing.get(key,"")) is not None,
                f"provider {rank} missing {key}")
    if rank < 2:
        require(timing.get("dataName","") not in {"","-"},
                f"provider {rank} missing dependency Data name")
        require("LLM_PIPELINE_QWEN_STAGE_OUTPUT" in log,
                f"provider {rank} missing stage output marker")
    else:
        require(timing.get("dataName") == "-",
                "final provider unexpectedly published an intermediate Data name")
        require("LLM_PIPELINE_QWEN_STAGE_FINAL" in log,
                "final provider missing final marker")
    if rank > 0:
        complete=[line for line in log.splitlines()
                  if "NDNSF_COLLAB_LARGE_FETCH_TIMING event=complete" in line]
        require(complete, f"provider {rank} missing completed dependency fetch")
    providers.append({"rank":rank,"node":nodes[rank],
                      "log":f"node-{rank}/provider-{rank}.log",
                      "timing":timing})
    timings.append(timing)

require(timings[0]["output_sha256"] == timings[1]["input_sha256"],
        "stage 0 output digest does not match stage 1 input digest")
require(timings[1]["output_sha256"] == timings[2]["input_sha256"],
        "stage 1 output digest does not match stage 2 input digest")
require(timings[0]["dataName"] != timings[1]["dataName"],
        "intermediate dependency Data names are not unique")

user_log=(root/"node-0/user.log").read_text(encoding="utf-8", errors="replace")
response_lines=[line for line in user_log.splitlines()
                if "LLM_PIPELINE_USER_RESPONSE" in line]
require(len(response_lines) == 1,
        f"expected one successful user response, got {len(response_lines)}")
response_line=response_lines[0]
json_start=response_line.find("{")
require(json_start >= 0, "user response marker has no JSON payload")
response=json.loads(response_line[json_start:])
expected_ranges=[[int(a),int(b)] for a,b in runtime["layerRanges"]]
require(response.get("topToken") == runtime["expectedTopToken"],
        f"top token mismatch: {response.get('topToken')} != {runtime['expectedTopToken']}")
require(response.get("stageCount") == 3, "response stageCount is not 3")
require(response.get("layerRanges") == expected_ranges,
        f"layer ranges mismatch: {response.get('layerRanges')} != {expected_ranges}")
require(response.get("logitsShape") and len(response["logitsShape"]) == 3,
        f"invalid logits shape: {response.get('logitsShape')}")
require("LLM_PIPELINE_USER_SUMMARY count=1" in user_log,
        "user summary does not report exactly one measured request")
require((root/"node-0/user-exit.txt").read_text().strip() == "0",
        "user process exit status is nonzero")
metrics=list(csv.DictReader((root/"node-0/user-metrics.csv").open()))
require(len(metrics) == 1 and metrics[0].get("status") == "ok",
        f"unexpected user metrics: {metrics}")

doc={"schemaVersion":"spec160-qwen-live-analysis-v2","status":"PASS",
     "runtimeContract":"clean-sif","nodes":nodes,"gpus":gpus,
     "providers":providers,"userLog":"node-0/user.log",
     "expectedTopToken":runtime["expectedTopToken"],
     "actualTopToken":response["topToken"],
     "logitsShape":response["logitsShape"],
     "layerRanges":response["layerRanges"],
     "dependencyDigestChain":[
       {"producerStage":0,"consumerStage":1,
        "sha256":timings[0]["output_sha256"],
        "dataName":timings[0]["dataName"]},
       {"producerStage":1,"consumerStage":2,
        "sha256":timings[1]["output_sha256"],
        "dataName":timings[1]["dataName"]}]}
out.write_text(json.dumps(doc,indent=2,sort_keys=True)+"\n")
PY
terminal_state=PASS
