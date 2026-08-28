#!/bin/bash
set -Eeuo pipefail

export NDN_CLIENT_TRANSPORT='unix:///scratch/run/nfd.sock'
export PYTHONNOUSERSITE=1
export PYTHONDONTWRITEBYTECODE=1
export NDN_LOG="${NDN_LOG:-ndn_service_framework.*=INFO}"
export HOME=/scratch/home
export NDNSF_CONFIG=/scratch/ndnsf.conf
export NDNSF_SESSION_BASE="${SLURM_JOB_ID:-0}"

mkdir -p /scratch/run /scratch/home /scratch/log

cleanup()
{
  rc=$?
  trap - EXIT INT TERM
  for pid in "${provider_pid:-}" "${controller_pid:-}"; do
    if [ -n "$pid" ]; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
  nfdc status report > /scratch/log/nfd-status-exit.txt 2>/dev/null || true
  if [ -n "${nfd_pid:-}" ]; then
    kill "$nfd_pid" 2>/dev/null || true
    wait "$nfd_pid" 2>/dev/null || true
  fi
  exit "$rc"
}
trap cleanup EXIT INT TERM

nfd --config /scratch/nfd.conf > /scratch/log/nfd.log 2>&1 &
nfd_pid=$!
for _ in $(seq 1 100); do
  test -S /scratch/run/nfd.sock && nfdc status >/dev/null 2>&1 && break
  kill -0 "$nfd_pid" 2>/dev/null || { echo NFD_EXITED_EARLY >&2; exit 5; }
  sleep 0.1
done
test -S /scratch/run/nfd.sock
nfdc status report > /scratch/log/nfd-status-initial.txt

cd /source
/opt/venv/bin/python examples/python_token_certificate_bootstrap_smoke.py \
  --role controller > /scratch/log/controller.log 2>&1 &
controller_pid=$!

deadline=$((SECONDS + 20))
while [ "$SECONDS" -lt "$deadline" ]; do
  if grep -q "ServiceController listening on:" /scratch/log/controller.log 2>/dev/null; then
    break
  fi
  kill -0 "$controller_pid" 2>/dev/null || {
    echo CONTROLLER_EXITED_EARLY >&2
    tail -n 120 /scratch/log/controller.log >&2 || true
    exit 6
  }
  sleep 0.1
done
grep -q "ServiceController listening on:" /scratch/log/controller.log

/opt/venv/bin/python examples/python_token_certificate_bootstrap_smoke.py \
  --role provider > /scratch/log/provider.log 2>&1 &
provider_pid=$!

deadline=$((SECONDS + 25))
while [ "$SECONDS" -lt "$deadline" ]; do
  if grep -Eq "registered service prefix=/HELLO|Registered service handler for /HELLO" /scratch/log/provider.log 2>/dev/null; then
    break
  fi
  kill -0 "$provider_pid" 2>/dev/null || {
    echo PROVIDER_EXITED_EARLY >&2
    tail -n 160 /scratch/log/provider.log >&2 || true
    exit 7
  }
  sleep 0.1
done
grep -Eq "registered service prefix=/HELLO|Registered service handler for /HELLO" /scratch/log/provider.log

timeout 30s /opt/venv/bin/python examples/python_token_certificate_bootstrap_smoke.py \
  --role user --label SPEC160_RUNTIME_SMOKE_REQUEST \
  > /scratch/log/user.log 2>&1
grep -q "SPEC160_RUNTIME_SMOKE_REQUEST=OK" /scratch/log/user.log

/opt/venv/bin/python - <<'PY' > /scratch/log/imports.log 2>&1
import json
import ndnsf
import py_repoclient
import ndnsf_distributed_inference
import ndnsf_distributed_inference.app_sdk.client
import ndnsf_distributed_inference.app_sdk.controller
import ndnsf_distributed_inference.app_sdk.provider
import ndnsf_distributed_inference.adapters.qwen
import ndnsf_distributed_inference.core
import ndnsf_distributed_inference.ops
import ndnsf_distributed_inference.planner
import ndnsf_distributed_inference.sdk
import onnxruntime
import torch
import transformers
print(json.dumps({
  "status": "PASS",
  "ndnsf": "present",
  "py_repoclient": "present",
  "ndnsf_distributed_inference": "present",
  "ndnsf_distributed_inference.app_sdk.client": "present",
  "ndnsf_distributed_inference.app_sdk.controller": "present",
  "ndnsf_distributed_inference.app_sdk.provider": "present",
  "ndnsf_distributed_inference.adapters.qwen": "present",
  "ndnsf_distributed_inference.core": "present",
  "ndnsf_distributed_inference.ops": "present",
  "ndnsf_distributed_inference.planner": "present",
  "ndnsf_distributed_inference.sdk": "present",
  "onnxruntime": onnxruntime.__version__,
  "torch": torch.__version__,
  "transformers": transformers.__version__,
}, sort_keys=True))
PY
grep -q '"status": "PASS"' /scratch/log/imports.log

if grep -RE "double free|invalid pointer|munmap_chunk|Aborted|core dumped" /scratch/log; then
  echo ALLOCATOR_CORRUPTION_OBSERVED >&2
  exit 8
fi

python3 - <<'PY' > /scratch/log/runtime-smoke-summary.json
import json, pathlib
root = pathlib.Path('/scratch/log')
summary = {
  'schemaVersion': 'spec160-runtime-smoke-v1',
  'status': 'PASS',
  'requestOk': 'SPEC160_RUNTIME_SMOKE_REQUEST=OK' in (root / 'user.log').read_text(errors='replace'),
  'controllerReady': 'ServiceController listening on:' in (root / 'controller.log').read_text(errors='replace'),
  'providerReady': any(token in (root / 'provider.log').read_text(errors='replace')
                       for token in ('registered service prefix=/HELLO',
                                     'Registered service handler for /HELLO')),
  'allocatorCorruption': False,
}
print(json.dumps(summary, indent=2, sort_keys=True))
PY
