#!/bin/bash
set -Eeuo pipefail
: "${SPEC166_RANK:?}"
: "${SPEC166_PORT:?}"
: "${SPEC166_RUN_ID:?}"

rank=$SPEC166_RANK
pids=()
cleanup()
{
  rc=$?
  trap - EXIT INT TERM
  for pid in "${pids[@]:-}"; do kill "$pid" 2>/dev/null || true; done
  nfdc status report > /scratch/log/nfd-status-exit.txt 2>/dev/null || true
  if test -n "${nfd_pid:-}"; then kill "$nfd_pid" 2>/dev/null || true; fi
  wait "${nfd_pid:-}" 2>/dev/null || true
  exit "$rc"
}
trap cleanup EXIT INT TERM

wait_file()
{
  local path=$1 attempts="${2:-1800}"
  for _ in $(seq 1 "$attempts"); do
    test -f "$path" && return 0
    sleep 0.1
  done
  echo "BARRIER_TIMEOUT:$path" >&2
  return 5
}
wait_all()
{
  local stem=$1
  wait_file "/shared/${stem}-0"
  wait_file "/shared/${stem}-1"
  wait_file "/shared/${stem}-2"
}
wait_log()
{
  local path=$1 marker=$2 pid=$3 attempts="${4:-6000}"
  for _ in $(seq 1 "$attempts"); do
    grep -Fq "$marker" "$path" 2>/dev/null && return 0
    kill -0 "$pid" 2>/dev/null || return 6
    sleep 0.1
  done
  echo "PROGRESS_TIMEOUT:$marker:$path" >&2
  return 5
}
bootstrap_token_for_identity()
{
  awk -v id="$1" '
    $0 !~ /^[[:space:]]*#/ && $1 == id { print $2; found=1; exit }
    END { if (!found) exit 1 }
  ' "$2"
}
run_nfdc()
{
  timeout 30s nfdc "$@"
}

nfd --config /scratch/nfd.conf > /scratch/log/nfd.log 2>&1 &
nfd_pid=$!
export NDN_CLIENT_TRANSPORT=unix:///scratch/run/nfd.sock
for _ in $(seq 1 200); do
  test -S /scratch/run/nfd.sock && run_nfdc status >/dev/null 2>&1 && break
  kill -0 "$nfd_pid" 2>/dev/null || exit 5
  sleep 0.1
done
test -S /scratch/run/nfd.sock
touch "/shared/nfd-ready-${rank}"
wait_all nfd-ready

for peer_rank in 0 1 2; do
  test "$peer_rank" -eq "$rank" && continue
  peer_ip=$(</shared/node-${peer_rank}/ipv4.txt)
  uri="tcp4://${peer_ip}:${SPEC166_PORT}"
  run_nfdc face create "$uri"
  run_nfdc route add /example/llm-pipeline "$uri" origin 65 cost 0
  run_nfdc route add /activation/llm "$uri" origin 65 cost 0
done
run_nfdc strategy set /example/llm-pipeline /localhost/nfd/strategy/multicast
run_nfdc strategy set /activation/llm /localhost/nfd/strategy/multicast
touch "/shared/routes-ready-${rank}"
wait_all routes-ready
sleep 5

export LD_LIBRARY_PATH="/opt/ndn-base/lib:/opt/ndnsf-app/lib:${LD_LIBRARY_PATH:-}"
export PYTHONPATH="/source:/source/llm_pipeline:/opt/ndnsf-app/python:${PYTHONPATH:-}"
export HF_HOME=/scratch/hf-home
export TRANSFORMERS_OFFLINE=1
export HF_HUB_OFFLINE=1
export PYTHONUNBUFFERED=1
export NDN_LOG="${NDN_LOG:-ndn_service_framework.*=DEBUG:ndnsf.*=DEBUG}"
export NDNSF_COLLAB_LARGE_FETCH_TIMING=1

if test "$rank" -eq 0; then
  /opt/venv/bin/python - <<'PY' > /scratch/log/controller.log 2>&1 &
from ndnsf_distributed_inference.app_sdk.controller import APPController
import subprocess
controller = APPController.from_config(
    "/input/policy.yaml",
    generated_policy_dir="/scratch/generated/controller",
    bootstrap_token_file="/shared/bootstrap-tokens.txt",
)
with open("/shared/controller.cert", "wb") as cert_out:
    subprocess.run(
        ["ndnsec", "cert-dump", "-i", "/example/llm-pipeline/controller"],
        stdout=cert_out, check=True)
print("SPEC166_CONTROLLER_READY", flush=True)
controller.run()
PY
  pids+=($!)
  controller_pid=${pids[0]}
  wait_log /scratch/log/controller.log SPEC166_CONTROLLER_READY "$controller_pid" 1200
  touch /shared/controller-ready
else
  wait_file /shared/controller-ready 1200
fi

case "$rank" in
  0) provider_id=""; provider_identity="/example/llm-pipeline/provider"; role="/LLM/Pipeline/Stage/0" ;;
  1) provider_id="1"; provider_identity="/example/llm-pipeline/provider/1"; role="/LLM/Pipeline/Stage/1" ;;
  2) provider_id="2"; provider_identity="/example/llm-pipeline/provider/2"; role="/LLM/Pipeline/Stage/2" ;;
esac
wait_file /shared/bootstrap-tokens.txt 1200
wait_file /shared/controller.cert 1200
export NDNSF_CONTROLLER_CERT_FILE=/shared/controller.cert
bootstrap_token=$(bootstrap_token_for_identity \
  "$provider_identity" /shared/bootstrap-tokens.txt)
/opt/venv/bin/python /source/llm_pipeline/provider.py \
  --config /input/policy.yaml \
  --generated-policy-dir "/scratch/generated/provider-${rank}" \
  --group /example/llm-pipeline/group \
  --provider-id "$provider_id" \
  --bootstrap-token "$bootstrap_token" \
  --roles "$role" \
  --runtime qwen-onnx \
  --stages 3 \
  --device cuda:0 \
  --require-cuda \
  --handler-workers 2 \
  --compute-delay-ms 0 \
  > "/scratch/log/provider-${rank}.log" 2>&1 &
provider_pid=$!
pids+=("$provider_pid")
wait_log "/scratch/log/provider-${rank}.log" \
  LLM_PIPELINE_PROVIDER_READY "$provider_pid" 6000
touch "/shared/provider-ready-${rank}"
wait_all provider-ready

if test "$rank" -eq 0; then
  touch /shared/user-started
  user_rc=0
  /opt/venv/bin/python /source/llm_pipeline/user.py \
    --config /input/policy.yaml \
    --generated-policy-dir /scratch/generated/user \
    --group /example/llm-pipeline/group \
    --runtime qwen-onnx \
    --stages 3 \
    --generation-campaign-manifest /input/generation-campaign.json \
    --generation-jsonl /scratch/log/generation.jsonl \
    --qwen-tokenizer-dir /input/model \
    --max-new-tokens 8 \
    --request-id "$SPEC166_RUN_ID" \
    --ack-timeout-ms 10000 \
    --timeout-ms 300000 \
    --workload-digest sha256:2d2aac62e9340ed401b7e7579d992f7fe652de06ee0829e0d8d0ea4c653a7ae9 \
    --model-identity-digest sha256:5ce2a6d5d0e96dea66cc439b6443460660cd8d99ad1ae84e7139033349851e7a \
    --campaign-id spec165-qwen3-minimum \
    --deployment-revision sha256:5ce2a6d5d0e96dea66cc439b6443460660cd8d99ad1ae84e7139033349851e7a \
    --app-state-root /scratch/app-state \
    --test-only-allow-ephemeral-app-state \
    > /scratch/log/user.log 2>&1 || user_rc=$?
  printf '%s\n' "$user_rc" > /scratch/log/user-exit.txt
  # Every provider has already consumed its identity-scoped bootstrap token,
  # and the user process has now terminated.  Remove the shared plaintext
  # token file before advertising campaign completion so the outer evidence
  # gate can fail closed on any credential residue.
  rm -f -- /shared/bootstrap-tokens.txt
  touch /shared/user-done
  test "$user_rc" -eq 0
else
  wait_file /shared/user-started 1200
  wait_file /shared/user-done 18000
fi
touch "/shared/rank-complete-${rank}"
