#!/usr/bin/env bash
# One real NDNSF Provider advertises exactly two GPUs. Two independent roles
# run ONNX Runtime CUDA, while two ranks of one logical role perform NCCL SUM.

set -u

ROOT=/scratch
LOG="$ROOT/log"
STATUS="$ROOT/status"
KEYCHAIN_ROOT="$ROOT/keychains"
BUNDLE=$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd) || {
  echo SPEC170_D2A_BUNDLE_RESOLVE_FAIL >&2
  exit 20
}
EVIDENCE="/evidence/spec170/d2a-local-two-gpu-${SLURM_JOB_ID:-incontainer}"
mkdir -p "$ROOT/run" "$LOG" "$STATUS" "$KEYCHAIN_ROOT" "$EVIDENCE"
cd "$BUNDLE"

required=(
  nfd.conf controller.policies trust-schema.conf
  spec170_v3_local_two_gpu_provider.py
  spec170_v3_local_two_gpu_user.py
  artifacts/qwen-native-tracer-backbone.onnx
)
for path in "${required[@]}"; do
  test -f "$BUNDLE/$path" || {
    echo "SPEC170_D2A_BUNDLE_FAIL missing=$BUNDLE/$path" >&2
    exit 20
  }
done

prepare_keychain() {
  local role=$1
  mkdir -p "$KEYCHAIN_ROOT/$role/pib" "$KEYCHAIN_ROOT/$role/tpm"
}
keychain_env() {
  local role=$1
  printf 'NDN_CLIENT_PIB=pib-sqlite3:%s NDN_CLIENT_TPM=tpm-file:%s' \
    "$KEYCHAIN_ROOT/$role/pib" "$KEYCHAIN_ROOT/$role/tpm"
}
for role in nfd nfdctl controller provider user; do prepare_keychain "$role"; done

export NDN_CLIENT_TRANSPORT=unix:///scratch/run/nfd.sock
export NDNSF_CONTROLLER_CERT_FILE=/scratch/controller.cert
export NDNSF_PROVIDER_HANDLER_THREADS=8
export NDNSF_PY_COLLAB_SELECTION_TRACE=1

cat >"$ROOT/bootstrap.tokens" <<'EOF'
# identity token role
/NDNSF-DI/Tracer/user user0001 user
/NDNSF-DI/Tracer/provider/local-two-gpu gpu20001 provider
EOF

PIDS=()
cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  for pid in "${PIDS[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
  sleep 1
  for pid in "${PIDS[@]}"; do kill -KILL "$pid" 2>/dev/null || true; done
  for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
  cp -a "$LOG" "$EVIDENCE/" 2>/dev/null || true
  cp -a "$STATUS" "$EVIDENCE/" 2>/dev/null || true
  cp -a "$ROOT/bootstrap.tokens" "$EVIDENCE/" 2>/dev/null || true
  printf 'exit=%s\nbundle=%s\nsifJob=%s\n' \
    "$rc" "$BUNDLE" "${SLURM_JOB_ID:-incontainer}" >"$EVIDENCE/manifest.txt"
  echo "SPEC170_D2A_WORKLOAD_TERMINAL exit=$rc bundle=$BUNDLE"
  exit "$rc"
}
trap cleanup EXIT INT TERM

read -r nfd_pib nfd_tpm < <(keychain_env nfd)
read -r nfdctl_pib nfdctl_tpm < <(keychain_env nfdctl)
read -r controller_pib controller_tpm < <(keychain_env controller)
read -r provider_pib provider_tpm < <(keychain_env provider)
read -r user_pib user_tpm < <(keychain_env user)

env "$nfd_pib" "$nfd_tpm" nfd --config "$BUNDLE/nfd.conf" \
  >"$LOG/nfd.log" 2>&1 &
nfd_pid=$!
PIDS+=("$nfd_pid")
nfd_start_ms=$(date +%s%3N)
nfd_ready=0
for _ in $(seq 1 600); do
  if env "$nfdctl_pib" "$nfdctl_tpm" nfdc status \
      >"$STATUS/nfdc-status.txt" 2>&1; then
    nfd_ready=1
    break
  fi
  if ! kill -0 "$nfd_pid" 2>/dev/null; then
    wait "$nfd_pid"; nfd_rc=$?
    echo "SPEC170_D2A_NFD_EXITED rc=$nfd_rc" | tee "$STATUS/nfd-start.txt"
    exit 10
  fi
  sleep 0.1
done
nfd_elapsed_ms=$(( $(date +%s%3N) - nfd_start_ms ))
printf 'ready=%s elapsedMs=%s pid=%s\n' "$nfd_ready" "$nfd_elapsed_ms" "$nfd_pid" \
  >"$STATUS/nfd-start.txt"
if [[ "$nfd_ready" -ne 1 ]]; then echo SPEC170_D2A_NFD_READY_FAIL; exit 10; fi
env "$nfdctl_pib" "$nfdctl_tpm" nfdc strategy set /NDNSF-DI/Tracer/group \
  /localhost/nfd/strategy/multicast >"$STATUS/strategy-set.txt" 2>&1 || {
  echo SPEC170_D2A_STRATEGY_FAIL
  exit 11
}

env "$controller_pib" "$controller_tpm" App_ServiceController \
  --policy-file "$BUNDLE/controller.policies" \
  --trust-schema "$BUNDLE/trust-schema.conf" \
  --controller-prefix /NDNSF-DI/Tracer/controller \
  --bootstrap-token-file "$ROOT/bootstrap.tokens" \
  >"$LOG/controller.log" 2>&1 &
controller_pid=$!
PIDS+=("$controller_pid")
controller_started=0
for _ in $(seq 1 1200); do
  if grep -q 'ServiceController started...' "$LOG/controller.log" 2>/dev/null; then
    controller_started=1
    break
  fi
  kill -0 "$controller_pid" 2>/dev/null || break
  sleep 0.1
done
if [[ "$controller_started" -ne 1 ]]; then
  echo SPEC170_D2A_CONTROLLER_START_FAIL
  exit 12
fi
controller_ready=0
for _ in $(seq 1 150); do
  if env "$controller_pib" "$controller_tpm" ndnsec cert-dump \
      -i /NDNSF-DI/Tracer/controller >"$ROOT/controller.cert.tmp" \
      2>"$STATUS/controller-cert.err" && test -s "$ROOT/controller.cert.tmp"; then
    mv "$ROOT/controller.cert.tmp" "$ROOT/controller.cert"
    controller_ready=1
    break
  fi
  sleep 0.1
done
if [[ "$controller_ready" -ne 1 ]]; then
  echo SPEC170_D2A_CONTROLLER_CERT_FAIL
  exit 13
fi

env "$provider_pib" "$provider_tpm" /opt/venv/bin/python \
  "$BUNDLE/spec170_v3_local_two_gpu_provider.py" \
  --artifact-root "$BUNDLE/artifacts" \
  --provider /NDNSF-DI/Tracer/provider/local-two-gpu \
  --group /NDNSF-DI/Tracer/group \
  --controller /NDNSF-DI/Tracer/controller \
  --trust-schema "$BUNDLE/trust-schema.conf" \
  --bootstrap-token gpu20001 \
  >"$LOG/provider.log" 2>&1 &
provider_pid=$!
PIDS+=("$provider_pid")
provider_ready=0
for _ in $(seq 1 1800); do
  if grep -q SPEC170_D2A_PROVIDER_READY "$LOG/provider.log" 2>/dev/null; then
    provider_ready=1
    break
  fi
  if ! kill -0 "$provider_pid" 2>/dev/null; then
    echo SPEC170_D2A_PROVIDER_EXITED
    tail -200 "$LOG/provider.log" 2>/dev/null || true
    break
  fi
  sleep 0.1
done
if [[ "$provider_ready" -ne 1 ]]; then
  echo SPEC170_D2A_PROVIDER_READY_FAIL
  exit 14
fi

run_user_case() {
  local case_name=$1
  local timeout_ms=$2
  local output="$LOG/user-${case_name}.log"
  env "$user_pib" "$user_tpm" /opt/venv/bin/python \
    "$BUNDLE/spec170_v3_local_two_gpu_user.py" \
    --artifact-root "$BUNDLE/artifacts" \
    --group /NDNSF-DI/Tracer/group \
    --controller /NDNSF-DI/Tracer/controller \
    --user /NDNSF-DI/Tracer/user \
    --trust-schema "$BUNDLE/trust-schema.conf" \
    --bootstrap-token user0001 \
    --provider /NDNSF-DI/Tracer/provider/local-two-gpu \
    --case "$case_name" \
    --request-id "spec170-v3-local-two-gpu-${case_name}" \
    --ack-timeout-ms 2500 --timeout-ms "$timeout_ms" \
    >"$output" 2>&1
}

run_user_case positive 20000
user_rc=$?
if [[ "$user_rc" -ne 0 ]]; then
  echo "SPEC170_D2A_USER_FAIL case=positive rc=$user_rc"
  exit 15
fi
grep -q SPEC170_D2A_USER_RESPONSE "$LOG/user-positive.log" || {
  echo SPEC170_D2A_USER_RESPONSE_MISSING
  exit 16
}
grep -q SPEC170_D2A_NUMERIC_ORACLE_PASS "$LOG/user-positive.log" || {
  echo SPEC170_D2A_ORACLE_MISSING
  exit 17
}
grep -q 'collectiveBackend=nccl' "$LOG/provider.log" || {
  echo SPEC170_D2A_NCCL_EVIDENCE_MISSING
  exit 18
}

run_user_case missing-rank 8000
user_rc=$?
if [[ "$user_rc" -ne 0 ]]; then
  echo "SPEC170_D2A_USER_FAIL case=missing-rank rc=$user_rc"
  exit 19
fi
grep -q 'SPEC170_D2A_GROUP_FAILURE_PASS case=missing-rank' \
  "$LOG/user-missing-rank.log" || {
  echo SPEC170_D2A_GROUP_FAILURE_MARKER_MISSING
  exit 20
}
if grep -q 'requestId=/spec170-v3-local-two-gpu-missing-rank.*complete=true' \
    "$LOG/provider.log"; then
  echo SPEC170_D2A_PARTIAL_RESPONSE_PUBLISHED
  exit 21
fi

echo "SPEC170_D2A_NETWORK_PASS job=${SLURM_JOB_ID:-incontainer} "\
"independentRoles=2 localRanks=2 collective=nccl groupFailure=PASS"
exit 0
