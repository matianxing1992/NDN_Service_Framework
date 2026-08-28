#!/bin/bash
set -Eeuo pipefail
: "${SPEC160_RANK:?}"
: "${SPEC160_PORT:?}"
: "${SPEC160_POLICY:?}"
: "${SPEC160_RUNTIME_SUMMARY:?}"
: "${SPEC160_REQUEST_ID:?}"

rank=$SPEC160_RANK
pids=()

cleanup()
{
  rc=$?
  trap - EXIT INT TERM
  for pid in "${pids[@]:-}"; do kill "$pid" 2>/dev/null || true; done
  nfdc status report > "/scratch/log/nfd-status-exit.txt" 2>/dev/null || true
  if test -n "${nfd_pid:-}"; then kill "$nfd_pid" 2>/dev/null || true; fi
  wait "${nfd_pid:-}" 2>/dev/null || true
  exit "$rc"
}
trap cleanup EXIT INT TERM

wait_file()
{
  path=$1
  local attempts="${2:-1800}"
  for _ in $(seq 1 "$attempts"); do
    test -f "$path" && return 0
    sleep 0.1
  done
  echo "BARRIER_TIMEOUT:$path" >&2
  return 5
}

wait_all()
{
  stem=$1
  wait_file "/shared/${stem}-0"
  wait_file "/shared/${stem}-1"
  wait_file "/shared/${stem}-2"
}

bootstrap_token_for_identity()
{
  identity=$1
  token_file=$2
  awk -v id="$identity" '
    $0 !~ /^[[:space:]]*#/ && $1 == id { print $2; found=1; exit }
    END { if (!found) exit 1 }
  ' "$token_file"
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
sleep 30

for peer_rank in 0 1 2; do
  test "$peer_rank" -eq "$rank" && continue
  peer_ip=$(</shared/node-${peer_rank}/ipv4.txt)
  uri="tcp4://${peer_ip}:${SPEC160_PORT}"
  run_nfdc face create "$uri"
  run_nfdc route add /NDNSF-DistributeInference/example "$uri" origin 65 cost 0
  run_nfdc route add /activation/llm "$uri" origin 65 cost 0
done
run_nfdc strategy set /NDNSF-DistributeInference/example /localhost/nfd/strategy/multicast
run_nfdc strategy set /activation/llm /localhost/nfd/strategy/multicast
touch "/shared/routes-ready-${rank}"
wait_all routes-ready
sleep 60

export LD_LIBRARY_PATH="/opt/ndn-base/lib:/opt/ndnsf-app/lib:${LD_LIBRARY_PATH:-}"
export PYTHONPATH="/source/llm_pipeline:/opt/ndnsf-app/python:${PYTHONPATH:-}"
export HF_HOME=/scratch/hf-home
export TRANSFORMERS_OFFLINE=1
export HF_HUB_OFFLINE=1
export PYTHONUNBUFFERED=1
export NDN_LOG="${NDN_LOG:-ndn_service_framework.*=DEBUG:ndnsf.*=DEBUG}"
export NDNSF_COLLAB_LARGE_FETCH_TIMING=1

if test "$rank" -eq 0; then
  /opt/venv/bin/python - <<'PY' > /scratch/log/controller.log 2>&1 &
from ndnsf_distributed_inference.app_sdk.controller import APPController
import os
import subprocess
controller = APPController.from_config(
    os.environ["SPEC160_POLICY"],
    generated_policy_dir="/scratch/generated/controller",
    bootstrap_token_file="/shared/bootstrap-tokens.txt",
)
with open("/shared/controller.cert", "wb") as cert_out:
    subprocess.run(
        [
            "ndnsec",
            "cert-dump",
            "-i",
            "/NDNSF-DistributeInference/example/controller",
        ],
        stdout=cert_out,
        check=True,
    )
print("SPEC160_CONTROLLER_READY", flush=True)
controller.run()
PY
  pids+=($!)
  touch /shared/controller-started
else
  wait_file /shared/controller-started
fi
sleep 20

case "$rank" in
  0) provider_id=""; provider_identity="/NDNSF-DistributeInference/example/provider"; role="/LLM/Pipeline/Stage/0" ;;
  1) provider_id="1"; provider_identity="/NDNSF-DistributeInference/example/provider/1"; role="/LLM/Pipeline/Stage/1" ;;
  2) provider_id="2"; provider_identity="/NDNSF-DistributeInference/example/provider/2"; role="/LLM/Pipeline/Stage/2" ;;
esac
wait_file /shared/bootstrap-tokens.txt 600
wait_file /shared/controller.cert 600
export NDNSF_CONTROLLER_CERT_FILE=/shared/controller.cert
bootstrap_token=$(bootstrap_token_for_identity "$provider_identity" /shared/bootstrap-tokens.txt)
/opt/venv/bin/python /source/llm_pipeline/provider.py \
  --config "$SPEC160_POLICY" \
  --generated-policy-dir "/scratch/generated/provider-${rank}" \
  --group /NDNSF-DistributeInference/example/group \
  --provider-id "$provider_id" \
  --bootstrap-token "$bootstrap_token" \
  --roles "$role" \
  --runtime qwen-transformers \
  --stages 3 \
  --device cuda:0 \
  --require-cuda \
  --handler-workers 2 \
  --compute-delay-ms 0 \
  > "/scratch/log/provider-${rank}.log" 2>&1 &
pids+=($!)
touch "/shared/provider-started-${rank}"
wait_all provider-started
sleep 120

if test "$rank" -eq 0; then
  touch /shared/user-started
  user_rc=0
  /opt/venv/bin/python /source/llm_pipeline/user.py \
    --config "$SPEC160_POLICY" \
    --generated-policy-dir /scratch/generated/user \
    --group /NDNSF-DistributeInference/example/group \
    --runtime qwen-transformers \
    --qwen-runtime-summary "$SPEC160_RUNTIME_SUMMARY" \
    --request-id "$SPEC160_REQUEST_ID" \
    --ack-timeout-ms 10000 \
    --timeout-ms 180000 \
    --measured-requests 1 \
    --metrics-csv /scratch/log/user-metrics.csv \
    --app-state-root /scratch/app-state \
    --test-only-allow-ephemeral-app-state \
    > /scratch/log/user.log 2>&1 || user_rc=$?
  printf '%s\n' "$user_rc" > /scratch/log/user-exit.txt
  touch /shared/user-done
  test "$user_rc" -eq 0
else
  wait_file /shared/user-started 6000
  wait_file /shared/user-done 6000
fi

touch "/shared/rank-complete-${rank}"
