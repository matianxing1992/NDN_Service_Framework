#!/usr/bin/env bash
# Candidate-bound Spec170 diagnostic: real NFD + controller + four native
# providers + Python NativeTracer user. This is not the packaged wrapper.
# Candidate defaults to the previous diagnostic release; override
# CANDIDATE_DIR/CANDIDATE_SOURCE through sbatch --export for a new immutable
# runtime. The bundle and SIF must both be staged under that candidate.
#SBATCH --job-name=spec170-native-network-smoke
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --time=00:15:00
#SBATCH --output=/project/tma1/ndnsf-di/logs/spec170-native-network-smoke-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/logs/spec170-native-network-smoke-%j.err

set -u
CANDIDATE_SOURCE=${CANDIDATE_SOURCE:-85d7aa475217cabbcfa92ad28612db99cd36e77b}
CANDIDATE_DIR=${CANDIDATE_DIR:-runtime-${CANDIDATE_SOURCE}}
ROOT=/project/tma1/ndnsf-di/evidence/spec170/${CANDIDATE_DIR}/network-bundle
SIF=/project/tma1/ndnsf-di/releases/spec170-${CANDIDATE_DIR}/runtime.sif
SCRATCH=/tmp/ndnsf-di-native-network-${SLURM_JOB_ID}
mkdir -p "$SCRATCH/run" "$SCRATCH/log" "$SCRATCH/cache" "$SCRATCH/status"
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
# Emit bounded native assignment/selection diagnostics for candidate smoke
# evidence. The output is retained with the run and is not a benchmark trace.
export APPTAINERENV_NDNSF_DI_RUNTIME_TIMING=1
export APPTAINERENV_NDNSF_PY_COLLAB_SELECTION_TRACE=1

container() {
  local name="$1"
  shift
  local home="$SCRATCH/home-$name"
  mkdir -p "$home"
  env -u APPTAINERENV_HOME -u SINGULARITYENV_HOME \
    apptainer exec --cleanenv --containall \
      --home "$home:/home/tma1" --pwd /tmp \
      --env NDN_CLIENT_TRANSPORT=unix:///scratch/run/nfd.sock \
      --env NDNSF_CONTROLLER_CERT_FILE=/scratch/controller.cert \
      --bind "$ROOT:/bundle:ro" --bind "$ROOT/artifacts:/artifacts:ro" \
      --bind "$SCRATCH:/scratch:rw" \
      --bind "$ROOT/user_driver.py:/scripts/user_driver.py:ro" "$SIF" "$@"
}

container_controller_home() {
  env -u APPTAINERENV_HOME -u SINGULARITYENV_HOME \
    apptainer exec --cleanenv --containall \
      --home "$SCRATCH/home-controller:/home/tma1" --pwd /tmp \
      --env NDN_CLIENT_TRANSPORT=unix:///scratch/run/nfd.sock \
      --bind "$ROOT:/bundle:ro" --bind "$ROOT/artifacts:/artifacts:ro" \
      --bind "$SCRATCH:/scratch:rw" "$SIF" "$@"
}

cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
  for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
  mkdir -p "$ROOT/network-smoke-$SLURM_JOB_ID"
  cp -a "$SCRATCH/log" "$ROOT/network-smoke-$SLURM_JOB_ID/" 2>/dev/null || true
  cp -a "$SCRATCH/status" "$ROOT/network-smoke-$SLURM_JOB_ID/" 2>/dev/null || true
  echo "SPEC170_NATIVE_NETWORK_SMOKE_TERMINAL exit=$rc job=$SLURM_JOB_ID"
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
  echo SPEC170_NATIVE_NETWORK_NFD_READY_FAIL
  cat "$SCRATCH/log/nfd.log"
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
  echo SPEC170_NATIVE_NETWORK_CONTROLLER_CERT_FAIL
  cat "$SCRATCH/controller-cert.err" 2>/dev/null || true
  exit 10
fi

provider() {
  local name="$1"
  local provider="$2"
  local role="$3"
  local bootstrap_token="$4"
  container "$name" /opt/ndnsf-di/current/bin/di-native-provider \
    --plan /bundle/native-execution-plan.json \
    --manifest /bundle/service-manifest.json \
    --service /Inference/NativeTracer --provider "$provider" --roles "$role" \
    --group /NDNSF-DI/Tracer/group \
    --controller /NDNSF-DI/Tracer/controller \
    --trust-schema /bundle/trust-schema.conf \
    --bootstrap-token "$bootstrap_token" \
    --artifact-cache-dir "/scratch/cache-$name" --serve \
    >"$SCRATCH/log/$name.log" 2>&1
  local rc=$?
  echo "$rc" >"$SCRATCH/status/$name.status"
  return "$rc"
}

for _ in $(seq 1 150); do
  grep -Eq 'ServiceController listening on:|ServiceController started' \
    "$SCRATCH/log/controller.log" 2>/dev/null && break
  sleep 0.1
done
provider backbone /NDNSF-DI/Tracer/provider/backbone /Backbone back0001 &
provider_pid=$!; PROVIDER_PIDS+=("$provider_pid"); PIDS+=("$provider_pid")
provider head0 /NDNSF-DI/Tracer/provider/head0 /Head/Shard/0 head0001 &
provider_pid=$!; PROVIDER_PIDS+=("$provider_pid"); PIDS+=("$provider_pid")
provider head1 /NDNSF-DI/Tracer/provider/head1 /Head/Shard/1 head1001 &
provider_pid=$!; PROVIDER_PIDS+=("$provider_pid"); PIDS+=("$provider_pid")
provider merge /NDNSF-DI/Tracer/provider/merge /Merge merge001 &
provider_pid=$!; PROVIDER_PIDS+=("$provider_pid"); PIDS+=("$provider_pid")

providers_ready=0
for _ in $(seq 1 300); do
  count=0
  for name in backbone head0 head1 merge; do
    grep -q 'NDNSF_DI_NATIVE_PROVIDER_READY' \
      "$SCRATCH/log/$name.log" 2>/dev/null && count=$((count + 1))
  done
  if [ "$count" -eq 4 ]; then
    providers_ready=1
    break
  fi
  sleep 0.1
done
if [ "$providers_ready" -ne 1 ]; then
  echo SPEC170_NATIVE_NETWORK_PROVIDER_READY_FAIL
  exit 11
fi
for pid in "${PROVIDER_PIDS[@]}"; do
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "SPEC170_NATIVE_NETWORK_PROVIDER_EXITED pid=$pid"
    exit 11
  fi
done

set +e
container user python3 /scripts/user_driver.py \
  --plan /bundle/native-execution-plan.json \
  --manifest /bundle/service-manifest.json \
  --service /Inference/NativeTracer \
  --group /NDNSF-DI/Tracer/group \
  --controller /NDNSF-DI/Tracer/controller \
  --user /NDNSF-DI/Tracer/user \
  --trust-schema /bundle/trust-schema.conf \
  --bootstrap-token user0001 \
  --permission-wait-ms 5000 --ack-timeout-ms 3000 \
  --timeout-ms 30000 --requests 1 \
  >"$SCRATCH/log/user.log" 2>&1 &
user_pid=$!
user_timed_out=0
deadline=$((SECONDS + 90))
while kill -0 "$user_pid" 2>/dev/null; do
  if [ "$SECONDS" -ge "$deadline" ]; then
    kill "$user_pid" 2>/dev/null || true
    user_timed_out=1
    break
  fi
  sleep 0.1
done
if [ "$user_timed_out" -eq 1 ]; then
  wait "$user_pid" 2>/dev/null || true
  user_rc=124
else
  wait "$user_pid"
  user_rc=$?
fi
set -u

for name in controller backbone head0 head1 merge user; do
  echo "=== $name ==="
  cat "$SCRATCH/log/$name.log" 2>/dev/null || true
done

if [ "$user_rc" -eq 0 ] \
   && grep -q 'NDNSF_DI_NATIVE_TRACER_USER_EXECUTION' "$SCRATCH/log/user.log" \
   && grep -q '"status": "executed"' "$SCRATCH/log/user.log"; then
  echo "SPEC170_NATIVE_NETWORK_SMOKE_PASS job=$SLURM_JOB_ID user_rc=$user_rc"
  exit 0
fi
echo "SPEC170_NATIVE_NETWORK_SMOKE_FAIL job=$SLURM_JOB_ID user_rc=$user_rc"
exit 12
