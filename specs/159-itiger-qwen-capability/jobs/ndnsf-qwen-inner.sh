#!/bin/bash
set -Eeuo pipefail
: "${SPEC159_REQUEST_ID:=spec159-qwen-live-001}"

export NDN_CLIENT_TRANSPORT='unix:///scratch/run/nfd.sock'
export NDNSF_CONFIG='/scratch/ndnsf.conf'
export NDNSF_SESSION_BASE="$((1700000000 + SLURM_JOB_ID))"
export NDNSF_DISABLE_NDNSD=1
export NDNSF_USE_ASYNC_SVS_PUBLISH=1
export NDNSF_SVS_PARALLEL_SYNC=1
export NDNSF_SVS_PARALLEL_WORKERS=1
export NDNSF_SVS_PARALLEL_PRODUCTION=1
export NDNSF_SVS_PARALLEL_PRODUCTION_SIGNING=0
export NDNSF_SVS_MAX_SUPPRESSION_MS=1
export NDN_LOG='ndn_service_framework.*=DEBUG'

mkdir -p /scratch/run /scratch/log
pids=()
finish()
{
  rc=$?
  trap - EXIT INT TERM
  for pid in "${pids[@]:-}"; do kill "$pid" 2>/dev/null || true; done
  wait "${pids[@]:-}" 2>/dev/null || true
  exit "$rc"
}
trap finish EXIT INT TERM

nfd --config /scratch/nfd.conf > /scratch/log/nfd.log 2>&1 &
nfd_pid=$!
pids+=("$nfd_pid")
for _ in $(seq 1 100); do
  if test -S /scratch/run/nfd.sock && nfdc status >/dev/null 2>&1; then break; fi
  kill -0 "$nfd_pid" 2>/dev/null || { echo NFD_EXITED_EARLY >&2; exit 5; }
  sleep 0.1
done
test -S /scratch/run/nfd.sock || { echo NFD_READINESS_TIMEOUT >&2; exit 5; }
for prefix in /example/hello /example/hello/group /example/hello/group/sync \
              /example/hello/group/s /example/hello/group/d; do
  nfdc strategy set "$prefix" /localhost/nfd/strategy/multicast/v=5 >/dev/null
done
nfdc status report > /scratch/log/nfd-status.txt

App_ServiceController \
  --policy-file /scratch/qwen.policies \
  --trust-schema /scratch/trust-schema.conf \
  > /scratch/log/controller.log 2>&1 &
pids+=("$!")
for _ in $(seq 1 100); do
  grep -q 'ServiceController started' /scratch/log/controller.log 2>/dev/null && break
  kill -0 "${pids[-1]}" 2>/dev/null || { echo CONTROLLER_EXITED_EARLY >&2; exit 6; }
  sleep 0.1
done
grep -q 'ServiceController started' /scratch/log/controller.log || {
  echo CONTROLLER_READINESS_TIMEOUT >&2; exit 6;
}

python3 /scratch/ndnsf-qwen-app.py provider \
  --trust-schema /scratch/trust-schema.conf \
  > /scratch/log/provider.log 2>&1 &
pids+=("$!")
for _ in $(seq 1 300); do
  grep -q 'SPEC159_NDNSF_PROVIDER_READY' /scratch/log/provider.log 2>/dev/null && break
  kill -0 "${pids[-1]}" 2>/dev/null || { echo PROVIDER_EXITED_EARLY >&2; exit 7; }
  sleep 0.1
done
grep -q 'SPEC159_NDNSF_PROVIDER_READY' /scratch/log/provider.log || {
  echo PROVIDER_READINESS_TIMEOUT >&2; exit 7;
}

timeout 45s python3 /scratch/ndnsf-qwen-app.py user \
  --trust-schema /scratch/trust-schema.conf \
  --request-id "$SPEC159_REQUEST_ID" \
  --output /scratch/ndnsf-result.json \
  > /scratch/log/user.log 2>&1

grep -q "SPEC159_QWEN_PROVIDER_EXEC requestId=${SPEC159_REQUEST_ID}" /scratch/log/provider.log
grep -q "SPEC159_QWEN_REQUESTER_RESULT requestId=${SPEC159_REQUEST_ID}" /scratch/log/user.log
grep -q 'Installed provider permission provider=/example/hello/provider/AI/LLM/Qwen service=/AI/LLM/Qwen' /scratch/log/provider.log
grep -q 'Installed user permission provider=/example/hello/provider/AI/LLM/Qwen service=/AI/LLM/Qwen' /scratch/log/user.log
grep -q 'UserToken/ProviderToken runtime mode: enabled' /scratch/log/provider.log
grep -q 'UserToken/ProviderToken runtime mode: enabled' /scratch/log/user.log
grep -q '\[ServiceProvider\] ACK publish requestId=' /scratch/log/provider.log
grep -q 'PublishServiceSelectionMessageV2:' /scratch/log/user.log
grep -q 'Received compact Service Selection Message:' /scratch/log/provider.log
grep -q 'OnResponse:' /scratch/log/user.log
grep -q '\[PERMISSIONS/USER\] Encrypted reply target=/example/hello/user' /scratch/log/controller.log
grep -q '\[PERMISSIONS/PROVIDER\] Encrypted reply target=/example/hello/provider' /scratch/log/controller.log
