#!/bin/bash
set -Eeuo pipefail

pids=()
cleanup()
{
  rc=$?
  trap - EXIT INT TERM
  for pid in "${pids[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  for pid in "${pids[@]:-}"; do
    wait "$pid" 2>/dev/null || true
  done
  rm -f -- /scratch/bootstrap-tokens.txt
  rm -rf -- /scratch/home/.ndn /scratch/app-state
  exit "$rc"
}
trap cleanup EXIT INT TERM

wait_log()
{
  file=$1
  marker=$2
  attempts=${3:-300}
  for _ in $(seq 1 "$attempts"); do
    grep -Fq "$marker" "$file" 2>/dev/null && return 0
    sleep 0.1
  done
  echo "LOG_MARKER_TIMEOUT file=$file marker=$marker" >&2
  tail -n 100 "$file" >&2 || true
  return 1
}

bootstrap_token_for_identity()
{
  identity=$1
  awk -v id="$identity" '
    $0 !~ /^[[:space:]]*#/ && $1 == id { print $2; found=1; exit }
    END { if (!found) exit 1 }
  ' /scratch/bootstrap-tokens.txt
}

mkdir -p /scratch/run /scratch/generated/controller \
  /scratch/generated/provider /scratch/generated/user /scratch/app-state
sed -e 's|@@NFD_SOCKET@@|/scratch/run/nfd.sock|g' \
    -e 's|@@PORT@@|6363|g' \
    /harness/nfd.conf.in > /scratch/nfd.conf

export HOME=/scratch/home
export NDN_CLIENT_TRANSPORT=unix:///scratch/run/nfd.sock
export LD_LIBRARY_PATH="/opt/ndn-base/lib:/opt/ndnsf-app/lib:${LD_LIBRARY_PATH:-}"
export PYTHONPATH="/source/llm_pipeline:/opt/ndnsf-app/python"
export PYTHONUNBUFFERED=1
export NDN_LOG="ndn_service_framework.*=INFO"

nfd --config /scratch/nfd.conf > /scratch/nfd.log 2>&1 &
pids+=($!)
for _ in $(seq 1 200); do
  test -S /scratch/run/nfd.sock && nfdc status >/dev/null 2>&1 && break
  sleep 0.1
done
test -S /scratch/run/nfd.sock

/opt/venv/bin/python - <<'PY' > /scratch/controller.log 2>&1 &
from ndnsf_distributed_inference.app_sdk.controller import APPController
import subprocess

controller = APPController.from_config(
    "/harness/local-docker-operation-status-policy.yaml",
    generated_policy_dir="/scratch/generated/controller",
    bootstrap_token_file="/scratch/bootstrap-tokens.txt",
)
with open("/scratch/controller.cert", "wb") as cert_out:
    subprocess.run(
        ["ndnsec", "cert-dump", "-i",
         "/NDNSF-DistributeInference/example/controller"],
        stdout=cert_out,
        check=True,
    )
print("SPEC160_LOCAL_CONTROLLER_READY", flush=True)
controller.run()
PY
pids+=($!)
wait_log /scratch/controller.log SPEC160_LOCAL_CONTROLLER_READY 600
test -s /scratch/controller.cert
test -s /scratch/bootstrap-tokens.txt

export NDNSF_CONTROLLER_CERT_FILE=/scratch/controller.cert
provider_token=$(
  bootstrap_token_for_identity /NDNSF-DistributeInference/example/provider
)
/opt/venv/bin/python /source/llm_pipeline/provider.py \
  --config /harness/local-docker-operation-status-policy.yaml \
  --generated-policy-dir /scratch/generated/provider \
  --group /NDNSF-DistributeInference/example/group \
  --bootstrap-token "$provider_token" \
  --roles /LLM/Pipeline/Stage/0 \
  --runtime fake \
  --stages 1 \
  --handler-workers 1 \
  --compute-delay-ms 0 \
  > /scratch/provider.log 2>&1 &
pids+=($!)
wait_log /scratch/provider.log LLM_PIPELINE_PROVIDER_READY 900

/opt/venv/bin/python /source/llm_pipeline/user.py \
  --config /harness/local-docker-operation-status-policy.yaml \
  --generated-policy-dir /scratch/generated/user \
  --group /NDNSF-DistributeInference/example/group \
  --runtime fake \
  --stages 1 \
  --request-id spec160-local-operation-status-smoke \
  --ack-timeout-ms 1500 \
  --timeout-ms 30000 \
  --measured-requests 1 \
  --metrics-csv /scratch/user-metrics.csv \
  --app-state-root /scratch/app-state \
  --test-only-allow-ephemeral-app-state \
  > /scratch/user.log 2>&1

grep -Fq LLM_PIPELINE_STAGE_FINAL /scratch/provider.log
awk -F, 'NR > 1 && $1 == "measured" && $4 == "ok" { found = 1 } END { exit !found }' \
  /scratch/user-metrics.csv
if grep -Fq "is not an instance of 'none'" /scratch/provider.log; then
  echo SPEC160_OPERATION_STATUS_BINDING_REGRESSION >&2
  exit 1
fi
for state in \
  "state=1" \
  "state=2" \
  "state=3" \
  "state=4"
do
  grep -F NDNSF_SELECTION_STATUS /scratch/provider.log | grep -Fq "$state"
done

printf '%s\n' SPEC160_LOCAL_DOCKER_OPERATION_STATUS_SMOKE_PASS
