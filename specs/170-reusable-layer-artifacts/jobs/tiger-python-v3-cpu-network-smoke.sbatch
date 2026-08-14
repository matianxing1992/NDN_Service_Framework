#!/usr/bin/env bash
# Spec170 V3 CPU/no-GPU network gate: real NFD + C++ controller + four
# Python-binding Providers + Python-binding V3 User.  This intentionally does
# not use the legacy NativeTracer driver or the native V1 execution plan.
# The SIF remains candidate-bound; the two diagnostic scripts are staged and
# hashed with the network evidence.
#SBATCH --job-name=spec170-python-v3-cpu
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --time=00:15:00
#SBATCH --output=/project/tma1/ndnsf-di/logs/spec170-python-v3-cpu-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/logs/spec170-python-v3-cpu-%j.err

set -u
CANDIDATE_SOURCE=${CANDIDATE_SOURCE:-2a7b847bbeba1b628a1a9e2a146b5411f1df5556}
CANDIDATE_DIR=${CANDIDATE_DIR:-runtime-${CANDIDATE_SOURCE}}
ROOT=/project/tma1/ndnsf-di/evidence/spec170/${CANDIDATE_DIR}/network-bundle
SIF=/project/tma1/ndnsf-di/releases/spec170-${CANDIDATE_DIR}/runtime.sif
SCRATCH=/tmp/ndnsf-di-python-v3-${SLURM_JOB_ID}
SPEC170_V3_ONNX=${SPEC170_V3_ONNX:-0}
SPEC170_V3_DEVICE=${SPEC170_V3_DEVICE:-cpu}
# The Merge role publishes the final response as soon as its own execution
# completes.  Other selected roles may still be flushing their execution
# markers when the User process returns, so drain their bounded diagnostics
# before the EXIT trap tears down the Provider containers.
SPEC170_V3_EXECUTION_GRACE_MS=${SPEC170_V3_EXECUTION_GRACE_MS:-5000}
mkdir -p "$SCRATCH/run" "$SCRATCH/log" "$SCRATCH/status"
if [ "$SPEC170_V3_DEVICE" != cpu ]; then
  nvidia-smi --query-gpu=uuid,name,driver_version,memory.total \
    --format=csv,noheader >"$SCRATCH/status/host-gpu.txt" 2>"$SCRATCH/status/host-gpu.err" || {
      echo SPEC170_PYTHON_V3_NETWORK_HOST_GPU_FAIL
      exit 9
    }
fi
cat >"$SCRATCH/bootstrap.tokens" <<'EOF'
# identity token role
/NDNSF-DI/Tracer/user user0001 user
/NDNSF-DI/Tracer/provider/backbone back0001 provider
/NDNSF-DI/Tracer/provider/head0 head0001 provider
/NDNSF-DI/Tracer/provider/head1 head1001 provider
/NDNSF-DI/Tracer/provider/merge merge001 provider
EOF
PIDS=()
PROVIDER_PIDS=()

container() {
  local name="$1"
  shift
  local home="$SCRATCH/home-$name"
  mkdir -p "$home"
  local gpu_args=()
  if [ "$SPEC170_V3_DEVICE" != cpu ]; then
    gpu_args=(--nv)
  fi
  env -u APPTAINERENV_HOME -u SINGULARITYENV_HOME \
    apptainer exec "${gpu_args[@]}" --cleanenv --containall \
      --home "$home:/home/tma1" --pwd /tmp \
      --env NDN_CLIENT_TRANSPORT=unix:///scratch/run/nfd.sock \
      --env NDNSF_CONTROLLER_CERT_FILE=/scratch/controller.cert \
      --env SPEC170_V3_HMAC_KEY=spec170-v3-network-diagnostic-key \
      --bind "$ROOT:/bundle:ro" \
      --bind "$SCRATCH:/scratch:rw" \
      --bind "$ROOT/spec170_v3_cpu_provider.py:/scripts/spec170_v3_cpu_provider.py:ro" \
      --bind "$ROOT/spec170_v3_cpu_user.py:/scripts/spec170_v3_cpu_user.py:ro" \
      "$SIF" "$@"
}

container_controller_home() {
  local gpu_args=()
  if [ "$SPEC170_V3_DEVICE" != cpu ]; then
    gpu_args=(--nv)
  fi
  env -u APPTAINERENV_HOME -u SINGULARITYENV_HOME \
    apptainer exec "${gpu_args[@]}" --cleanenv --containall \
      --home "$SCRATCH/home-controller:/home/tma1" --pwd /tmp \
      --env NDN_CLIENT_TRANSPORT=unix:///scratch/run/nfd.sock \
      --bind "$ROOT:/bundle:ro" --bind "$SCRATCH:/scratch:rw" \
      "$SIF" "$@"
}

cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
  for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
  mkdir -p "$ROOT/python-v3-network-smoke-$SLURM_JOB_ID"
  cp -a "$SCRATCH/log" "$ROOT/python-v3-network-smoke-$SLURM_JOB_ID/" 2>/dev/null || true
  cp -a "$SCRATCH/status" "$ROOT/python-v3-network-smoke-$SLURM_JOB_ID/" 2>/dev/null || true
  cp -a "$SCRATCH/bootstrap.tokens" "$ROOT/python-v3-network-smoke-$SLURM_JOB_ID/" 2>/dev/null || true
  echo "SPEC170_PYTHON_V3_NETWORK_TERMINAL exit=$rc job=$SLURM_JOB_ID"
  exit "$rc"
}
trap cleanup EXIT INT TERM

container nfd nfd --config /bundle/nfd.conf \
  >"$SCRATCH/log/nfd.log" 2>&1 & PIDS+=("$!")
ready=0
for _ in $(seq 1 100); do
  if container nfdctl nfdc status >"$SCRATCH/status/nfdc-status.txt" 2>&1; then
    ready=1
    break
  fi
  sleep 0.1
done
if [ "$ready" -ne 1 ]; then
  echo SPEC170_PYTHON_V3_NETWORK_NFD_READY_FAIL
  exit 10
fi
container nfdctl nfdc strategy set /NDNSF-DI/Tracer/group \
  /localhost/nfd/strategy/multicast >/dev/null 2>&1 || true
container nfdctl nfdc strategy set /NDNSF-DI/Tracer \
  /localhost/nfd/strategy/multicast >/dev/null 2>&1 || true

container controller /opt/ndnsf-di/current/bin/App_ServiceController \
  --policy-file /bundle/controller.policies \
  --trust-schema /bundle/trust-schema.conf \
  --controller-prefix /NDNSF-DI/Tracer/controller \
  --bootstrap-token-file /scratch/bootstrap.tokens \
  >"$SCRATCH/log/controller.log" 2>&1 & PIDS+=("$!")

controller_cert_ready=0
for _ in $(seq 1 100); do
  if container_controller_home ndnsec cert-dump -i /NDNSF-DI/Tracer/controller \
      >"$SCRATCH/controller.cert.tmp" 2>"$SCRATCH/controller-cert.err" \
      && test -s "$SCRATCH/controller.cert.tmp"; then
    mv "$SCRATCH/controller.cert.tmp" "$SCRATCH/controller.cert"
    controller_cert_ready=1
    break
  fi
  sleep 0.1
done
if [ "$controller_cert_ready" -ne 1 ]; then
  echo SPEC170_PYTHON_V3_NETWORK_CONTROLLER_CERT_FAIL
  exit 10
fi

provider() {
  local name="$1"
  local provider_identity="$2"
  local role="$3"
  local token="$4"
  local artifact="${5:-}"
  local runtime_args=()
  if [ "$SPEC170_V3_ONNX" = 1 ]; then
    runtime_args=(--onnx --artifact "$artifact" --device "$SPEC170_V3_DEVICE")
  else
    runtime_args=(--device "$SPEC170_V3_DEVICE")
  fi
  container "$name" python3 /scripts/spec170_v3_cpu_provider.py \
    --provider "$provider_identity" --role "$role" \
    --group /NDNSF-DI/Tracer/group \
    --controller /NDNSF-DI/Tracer/controller \
    --trust-schema /bundle/trust-schema.conf \
    --bootstrap-token "$token" "${runtime_args[@]}" \
    >"$SCRATCH/log/$name.log" 2>&1
  local status=$?
  echo "$status" >"$SCRATCH/status/$name.status"
  return "$status"
}

for _ in $(seq 1 150); do
  grep -Eq 'ServiceController listening on:|ServiceController started' \
    "$SCRATCH/log/controller.log" 2>/dev/null && break
  sleep 0.1
done
provider backbone /NDNSF-DI/Tracer/provider/backbone /Backbone back0001 \
  /bundle/artifacts/qwen-native-tracer-backbone.onnx &
provider_pid=$!; PROVIDER_PIDS+=("$provider_pid"); PIDS+=("$provider_pid")
provider head0 /NDNSF-DI/Tracer/provider/head0 /Head/Shard/0 head0001 \
  /bundle/artifacts/qwen-native-tracer-head0.onnx &
provider_pid=$!; PROVIDER_PIDS+=("$provider_pid"); PIDS+=("$provider_pid")
provider head1 /NDNSF-DI/Tracer/provider/head1 /Head/Shard/1 head1001 \
  /bundle/artifacts/qwen-native-tracer-head1.onnx &
provider_pid=$!; PROVIDER_PIDS+=("$provider_pid"); PIDS+=("$provider_pid")
provider merge /NDNSF-DI/Tracer/provider/merge /Merge merge001 \
  /bundle/artifacts/qwen-native-tracer-merge.onnx &
provider_pid=$!; PROVIDER_PIDS+=("$provider_pid"); PIDS+=("$provider_pid")

providers_ready=0
for _ in $(seq 1 300); do
  count=0
  for name in backbone head0 head1 merge; do
    grep -q 'SPEC170_V3_PROVIDER_READY' \
      "$SCRATCH/log/$name.log" 2>/dev/null && count=$((count + 1))
  done
  if [ "$count" -eq 4 ]; then
    providers_ready=1
    break
  fi
  sleep 0.1
done
if [ "$providers_ready" -ne 1 ]; then
  echo SPEC170_PYTHON_V3_NETWORK_PROVIDER_READY_FAIL
  exit 11
fi
if [ "$SPEC170_V3_ONNX" = 1 ]; then
  runtimes_ready=0
  for _ in $(seq 1 100); do
    count=0
    for name in backbone head0 head1 merge; do
      grep -q 'SPEC170_V3_PROVIDER_RUNTIME_READY' \
        "$SCRATCH/log/$name.log" 2>/dev/null && count=$((count + 1))
    done
    if [ "$count" -eq 4 ]; then
      runtimes_ready=1
      break
    fi
    sleep 0.1
  done
  if [ "$runtimes_ready" -ne 1 ]; then
    echo SPEC170_PYTHON_V3_NETWORK_RUNTIME_READY_FAIL
    exit 11
  fi
fi

set +e
container user python3 /scripts/spec170_v3_cpu_user.py \
  --group /NDNSF-DI/Tracer/group \
  --controller /NDNSF-DI/Tracer/controller \
  --user /NDNSF-DI/Tracer/user \
  --trust-schema /bundle/trust-schema.conf \
  --bootstrap-token user0001 \
  --device "$SPEC170_V3_DEVICE" \
  --ack-timeout-ms 1500 --timeout-ms 15000 \
  >"$SCRATCH/log/user.log" 2>&1
user_rc=$?
set -u

execution_count=0
if [ "$SPEC170_V3_ONNX" = 1 ]; then
  execution_count=0
  execution_polls=$((SPEC170_V3_EXECUTION_GRACE_MS / 100))
  if [ "$execution_polls" -lt 1 ]; then
    execution_polls=1
  fi
  for _ in $(seq 1 "$execution_polls"); do
    execution_count=0
    for name in backbone head0 head1 merge; do
      grep -q 'SPEC170_V3_PROVIDER_EXECUTION' \
        "$SCRATCH/log/$name.log" 2>/dev/null && execution_count=$((execution_count + 1))
    done
    [ "$execution_count" -eq 4 ] && break
    sleep 0.1
  done
  echo "SPEC170_PYTHON_V3_EXECUTION_DRAIN count=$execution_count graceMs=$SPEC170_V3_EXECUTION_GRACE_MS"
fi

for name in controller backbone head0 head1 merge user; do
  echo "=== $name ==="
  cat "$SCRATCH/log/$name.log" 2>/dev/null || true
done

if [ "$user_rc" -eq 0 ] \
   && grep -q 'SPEC170_V3_USER_ACK_CLOSED' "$SCRATCH/log/user.log" \
   && grep -q 'SPEC170_V3_USER_SELECTION_COMMITTED' "$SCRATCH/log/user.log" \
   && grep -q 'SPEC170_V3_USER_RESPONSE' "$SCRATCH/log/user.log" \
   && grep -q 'SPEC170_V3_PROVIDER_SELECTED' "$SCRATCH/log/backbone.log" \
   && grep -q 'SPEC170_V3_PROVIDER_SELECTED' "$SCRATCH/log/head0.log" \
   && grep -q 'SPEC170_V3_PROVIDER_SELECTED' "$SCRATCH/log/head1.log" \
   && grep -q 'SPEC170_V3_PROVIDER_RESPONSE' "$SCRATCH/log/merge.log" \
   && { [ "$SPEC170_V3_ONNX" != 1 ] || {
        [ "$execution_count" -eq 4 ];
      }; }; then
  if [ "$SPEC170_V3_ONNX" = 1 ] && [ "$SPEC170_V3_DEVICE" != cpu ]; then
    for name in backbone head0 head1 merge; do
      grep -Eq "SPEC170_V3_PROVIDER_RUNTIME_READY .*device=$SPEC170_V3_DEVICE .*providers=CUDAExecutionProvider[^ ]* .*cpuFallbackDisabled=true" \
        "$SCRATCH/log/$name.log" || {
          echo "SPEC170_PYTHON_V3_NETWORK_GPU_PROVIDER_FAIL name=$name"
          exit 13
        }
    done
  fi
  echo "SPEC170_PYTHON_V3_NETWORK_PASS job=$SLURM_JOB_ID user_rc=$user_rc"
  exit 0
fi
echo "SPEC170_PYTHON_V3_NETWORK_FAIL job=$SLURM_JOB_ID user_rc=$user_rc"
exit 12
