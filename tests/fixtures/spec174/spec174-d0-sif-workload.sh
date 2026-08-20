#!/usr/bin/env bash
# Candidate-bound Spec170 D0 workload for the exact sealed SIF supplied by the
# release wrapper.
#
# This is a real four-Provider NDNSF request, not a check-only or launch-only
# probe.  Each Provider runs the current di-native-provider executable with one
# role, the User runs the current native_di_tracer/user_driver.py, and the
# evidence requires the REQUEST -> ACK -> SELECTION -> RESPONSE path to finish.

set -u

ROOT=/scratch
LOG="$ROOT/log"
STATUS="$ROOT/status"
KEYCHAIN_ROOT="$ROOT/keychains"
BUNDLE="$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd || true)"
mkdir -p "$ROOT/run" "$LOG" "$STATUS" "$KEYCHAIN_ROOT"

prepare_keychain() {
  local role="$1"
  mkdir -p "$KEYCHAIN_ROOT/$role/pib" "$KEYCHAIN_ROOT/$role/tpm"
}

keychain_env() {
  local role="$1"
  printf 'NDN_CLIENT_PIB=pib-sqlite3:%s NDN_CLIENT_TPM=tpm-file:%s' \
    "$KEYCHAIN_ROOT/$role/pib" "$KEYCHAIN_ROOT/$role/tpm"
}

for role in nfd nfdctl controller backbone head0 head1 merge user; do
  prepare_keychain "$role"
done

required=(
  nfd.conf.in controller.policies trust-schema.conf
  native-execution-plan.json service-manifest.json user_driver.py
  artifacts/qwen-native-tracer-backbone.onnx
  artifacts/qwen-native-tracer-head0.onnx
  artifacts/qwen-native-tracer-head1.onnx
  artifacts/qwen-native-tracer-merge.onnx
)
for path in "${required[@]}"; do
  if [[ ! -f "$BUNDLE/$path" ]]; then
    echo "SPEC170_D0_CURRENT_BUNDLE_FAIL missing=$BUNDLE/$path"
    exit 20
  fi
done

sed -e 's|@@NFD_SOCKET@@|/scratch/run/nfd.sock|g' \
    -e 's|@@TCP_PORT@@|6363|g' \
    -e 's|@@UDP_PORT@@|6363|g' \
    -e '/^[[:space:]]*status[[:space:]]*$/d' \
    "$BUNDLE/nfd.conf.in" > "$ROOT/nfd.conf"

export NDN_CLIENT_TRANSPORT=unix:///scratch/run/nfd.sock
export NDNSF_CONTROLLER_CERT_FILE=/scratch/controller.cert
export NDNSF_PY_COLLAB_SELECTION_TRACE=1
export NDNSF_DI_RUNTIME_TIMING=1
export NDN_LOG='ndn_service_framework.ServiceUser=TRACE:ndn_service_framework.ServiceProvider=TRACE:ndn_service_framework.ServiceController=TRACE'
# Four CPU Providers share the D0 allocation. Keep each ONNX Runtime process
# bounded so startup/warmup does not oversubscribe the Slurm CPU cgroup.
export OMP_NUM_THREADS=1
export ORT_NUM_THREADS=1
export MKL_NUM_THREADS=1
export OPENBLAS_NUM_THREADS=1

cat > "$ROOT/bootstrap.tokens" <<'EOF'
# Retained for the controller's bootstrap-token-file compatibility path.
/NDNSF-DI/Tracer/user user0001 user
/NDNSF-DI/Tracer/provider/backbone back0001 provider
/NDNSF-DI/Tracer/provider/head0 head0001 provider
/NDNSF-DI/Tracer/provider/head1 head1001 provider
/NDNSF-DI/Tracer/provider/merge merge0001 provider
EOF

PIDS=()
cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  for pid in "${PIDS[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
  sleep 1
  # A failed readiness gate must not hang forever waiting for a Provider that
  # never reached its event-loop shutdown path.  Preserve logs, then reap
  # stubborn children with SIGKILL.
  for pid in "${PIDS[@]}"; do kill -KILL "$pid" 2>/dev/null || true; done
  for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
  local evidence="/evidence/spec170/d0-current-${SLURM_JOB_ID:-incontainer}"
  mkdir -p "$evidence"
  cp -a "$LOG" "$evidence/" 2>/dev/null || true
  cp -a "$STATUS" "$evidence/" 2>/dev/null || true
  cp -a "$ROOT/nfd.conf" "$ROOT/bootstrap.tokens" "$evidence/" 2>/dev/null || true
  printf 'SPEC170_D0_CURRENT_TERMINAL exit=%s bundle=%s\n' "$rc" "$BUNDLE"
  exit "$rc"
}
trap cleanup EXIT INT TERM

read -r nfd_pib nfd_tpm < <(keychain_env nfd)
read -r nfdctl_pib nfdctl_tpm < <(keychain_env nfdctl)
read -r controller_pib controller_tpm < <(keychain_env controller)

env "$nfd_pib" "$nfd_tpm" nfd --config "$ROOT/nfd.conf" \
  >"$LOG/nfd.log" 2>&1 & PIDS+=("$!")
nfd_ready=0
for _ in $(seq 1 120); do
  if env "$nfdctl_pib" "$nfdctl_tpm" nfdc status \
      >"$STATUS/nfdc-status.txt" 2>&1; then
    nfd_ready=1
    break
  fi
  sleep 0.1
done
if [[ "$nfd_ready" -ne 1 ]]; then
  echo SPEC170_D0_CURRENT_NFD_READY_FAIL
  exit 10
fi
env "$nfdctl_pib" "$nfdctl_tpm" nfdc strategy set /NDNSF-DI/Tracer/group \
  /localhost/nfd/strategy/multicast >/dev/null 2>&1 || true
env "$nfdctl_pib" "$nfdctl_tpm" nfdc strategy set /NDNSF-DI/Tracer \
  /localhost/nfd/strategy/multicast >/dev/null 2>&1 || true

env "$controller_pib" "$controller_tpm" App_ServiceController \
  --policy-file "$BUNDLE/controller.policies" \
  --trust-schema "$BUNDLE/trust-schema.conf" \
  --controller-prefix /NDNSF-DI/Tracer/controller \
  --bootstrap-token-file "$ROOT/bootstrap.tokens" \
  >"$LOG/controller.log" 2>&1 & PIDS+=("$!")
controller_pid="${PIDS[$(( ${#PIDS[@]} - 1 ))]}"
printf 'SPEC170_D0_CONTROLLER_PID=%s\n' "$controller_pid" >"$STATUS/controller-startup.txt"
controller_started=0
for _ in $(seq 1 1200); do
  # Do not open the Controller PIB while App_ServiceController is still
  # constructing its KeyChain.  An immediate ndnsec cert-dump races SQLite
  # initialization and aborts the otherwise valid Controller.
  if grep -q 'ServiceController started...' "$LOG/controller.log" 2>/dev/null; then
    controller_started=1
    break
  fi
  if ! kill -0 "$controller_pid" 2>/dev/null; then
    echo SPEC170_D0_CURRENT_CONTROLLER_EXITED >"$STATUS/controller-startup.txt"
    tail -200 "$LOG/controller.log" 2>/dev/null || true
    break
  fi
  sleep 0.1
done
if [[ "$controller_started" -ne 1 ]]; then
  echo SPEC170_D0_CURRENT_CONTROLLER_START_FAIL
  tail -200 "$LOG/controller.log" 2>/dev/null || true
  exit 10
fi
controller_ready=0
for _ in $(seq 1 120); do
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
  echo SPEC170_D0_CURRENT_CONTROLLER_CERT_FAIL
  exit 10
fi

provider() {
  local name="$1" identity="$2" role="$3" token="$4"
  local provider_pib provider_tpm
  read -r provider_pib provider_tpm < <(keychain_env "$name")
  # The function itself is backgrounded below.  `exec` must replace that
  # function shell, otherwise cleanup only kills an intermediate shell and
  # leaves the Provider executable orphaned in the host namespace.
  cd "$BUNDLE"
  exec env "$provider_pib" "$provider_tpm" \
    /opt/ndnsf-di/current/bin/di-native-provider \
    --plan "$BUNDLE/native-execution-plan.json" \
    --manifest "$BUNDLE/service-manifest.json" \
    --service /Inference/NativeTracer \
    --provider "$identity" --roles "$role" --workers 1 \
    --handler-threads 2 --ack-threads 2 \
    --group /NDNSF-DI/Tracer/group \
    --controller /NDNSF-DI/Tracer/controller \
    --trust-schema "$BUNDLE/trust-schema.conf" \
    --bootstrap-token "$token" \
    --artifact-cache-dir "$ROOT/cache-$name" \
    --serve >"$LOG/$name.log" 2>&1
}

provider backbone /NDNSF-DI/Tracer/provider/backbone /Backbone back0001 &
backbone_pid=$!; printf '%s\n' "$backbone_pid" >"$STATUS/backbone.pid"; PIDS+=("$backbone_pid")
provider head0 /NDNSF-DI/Tracer/provider/head0 /Head/Shard/0 head0001 &
head0_pid=$!; printf '%s\n' "$head0_pid" >"$STATUS/head0.pid"; PIDS+=("$head0_pid")
provider head1 /NDNSF-DI/Tracer/provider/head1 /Head/Shard/1 head1001 &
head1_pid=$!; printf '%s\n' "$head1_pid" >"$STATUS/head1.pid"; PIDS+=("$head1_pid")
provider merge /NDNSF-DI/Tracer/provider/merge /Merge merge0001 &
merge_pid=$!; printf '%s\n' "$merge_pid" >"$STATUS/merge.pid"; PIDS+=("$merge_pid")

providers_ready=0
for _ in $(seq 1 1800); do
  count=0
  alive=0
  for name in backbone head0 head1 merge; do
    grep -q 'NDNSF_DI_NATIVE_PROVIDER_READY' "$LOG/$name.log" 2>/dev/null \
      && count=$((count + 1))
    pid_file="$STATUS/$name.pid"
    if [[ -f "$pid_file" ]] && kill -0 "$(cat "$pid_file")" 2>/dev/null; then
      alive=$((alive + 1))
    fi
  done
  if [[ "$count" -eq 4 ]]; then
    providers_ready=1
    break
  fi
  if [[ "$alive" -gt 0 && "$alive" -lt 4 ]]; then
    echo "SPEC170_D0_CURRENT_PROVIDER_EARLY_EXIT ready=$count alive=$alive"
    for name in backbone head0 head1 merge; do
      echo "=== provider=$name ==="
      tail -200 "$LOG/$name.log" 2>/dev/null || true
    done
    break
  fi
  sleep 0.1
done
if [[ "$providers_ready" -ne 1 ]]; then
  echo SPEC170_D0_CURRENT_PROVIDER_READY_FAIL
  for name in backbone head0 head1 merge; do
    echo "=== provider=$name ==="
    tail -200 "$LOG/$name.log" 2>/dev/null || true
  done
  exit 11
fi

read -r user_pib user_tpm < <(keychain_env user)
set +e
env "$user_pib" "$user_tpm" python3 "$BUNDLE/user_driver.py" \
  --plan "$BUNDLE/native-execution-plan.json" \
  --service /Inference/NativeTracer \
  --group /NDNSF-DI/Tracer/group \
  --controller /NDNSF-DI/Tracer/controller \
  --user /NDNSF-DI/Tracer/user \
  --trust-schema "$BUNDLE/trust-schema.conf" \
  --bootstrap-token user0001 \
  --permission-wait-ms 5000 --ack-timeout-ms 1500 --timeout-ms 30000 \
  --requests 1 \
  --role-provider-preference \
  '/Backbone=>/NDNSF-DI/Tracer/provider/backbone;/Head/Shard/0=>/NDNSF-DI/Tracer/provider/head0;/Head/Shard/1=>/NDNSF-DI/Tracer/provider/head1;/Merge=>/NDNSF-DI/Tracer/provider/merge;' \
  >"$LOG/user.log" 2>&1
user_rc=$?
set -u

selection_ok=1
grep -q 'event=SELECTION_PUBLISHED' "$LOG/user.log" || selection_ok=0
for name in backbone head0 head1 merge; do
  grep -q 'event=SELECTION_RECEIVED' "$LOG/$name.log" || selection_ok=0
done

dependency_ok=1
for scope in backbone-to-head0 backbone-to-head1 head0-to-merge head1-to-merge; do
  grep -h "NDNSF_DI_DEPENDENCY_OUTPUT_TIMING.*scope=$scope " \
    "$LOG"/{backbone,head0,head1,merge}.log >/dev/null || dependency_ok=0
  grep -h "NDNSF_DI_DEPENDENCY_INPUT_TIMING.*scope=$scope " \
    "$LOG"/{backbone,head0,head1,merge}.log >/dev/null || dependency_ok=0
done

response_ok=0
if [[ "$user_rc" -eq 0 ]] \
   && [[ "$selection_ok" -eq 1 ]] \
   && [[ "$dependency_ok" -eq 1 ]] \
   && grep -q '"status": "executed"' "$LOG/user.log" \
   && grep -q 'NDNSF_PY_COLLAB_SELECTION' "$LOG/user.log" \
   && grep -q 'NDNSF_DI_FINAL_RESPONSE\|NDNSF_DI_NATIVE_FINAL_RESPONSE_DECISION' "$LOG/merge.log"; then
  response_ok=1
fi
printf 'user_rc=%s selection_ok=%s dependency_ok=%s response_ok=%s\n' \
  "$user_rc" "$selection_ok" "$dependency_ok" "$response_ok" \
  > "$STATUS/terminal-check.txt"

for name in backbone head0 head1 merge; do
  grep -q 'NDNSF_DI_NATIVE_PROVIDER_READY' "$LOG/$name.log" || response_ok=0
done
if [[ "$response_ok" -ne 1 ]]; then
  echo "SPEC170_D0_CURRENT_NETWORK_FAIL user_rc=$user_rc"
  exit 12
fi
echo "SPEC170_D0_CURRENT_NETWORK_PASS job=${SLURM_JOB_ID:-incontainer} request_ack_selection_response=PASS"
exit 0
