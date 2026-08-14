#!/usr/bin/env bash
# Candidate-bound workload for gate-d0-cpu.sbatch.
#
# This file runs *inside* run-container.sh.  It deliberately does not launch a
# nested Apptainer process: the exact SIF, clean environment, zero-GPU policy,
# and project mounts are owned by the outer D0 entrypoint.  The workload keeps
# the same four-role V3 control/selection/ONNX smoke as the Tiger diagnostic,
# but exercises it through the formal container launcher.

set -u

ROOT=/scratch
LOG="$ROOT/log"
STATUS="$ROOT/status"
mkdir -p "$ROOT/run" "$LOG" "$STATUS"

# The release bind contains exactly one staged Spec170 V3 network bundle for a
# candidate-bound D0 run.  Refuse ambiguity instead of silently choosing an
# older candidate.
BUNDLE=""
BUNDLE_COUNT=0
for candidate in /release/spec170-runtime-*/network-bundle; do
  if [ -f "$candidate/nfd.conf" ] \
     && [ -f "$candidate/controller.policies" ] \
     && [ -f "$candidate/trust-schema.conf" ] \
     && [ -f "$candidate/spec170_v3_cpu_provider.py" ] \
     && [ -f "$candidate/spec170_v3_cpu_user.py" ]; then
    BUNDLE="$candidate"
    BUNDLE_COUNT=$((BUNDLE_COUNT + 1))
  fi
done
if [ "$BUNDLE_COUNT" -ne 1 ]; then
  echo "SPEC170_D0_V3_BUNDLE_FAIL count=$BUNDLE_COUNT"
  exit 20
fi

export NDN_CLIENT_TRANSPORT=unix:///scratch/run/nfd.sock
export NDNSF_CONTROLLER_CERT_FILE=/scratch/controller.cert
export SPEC170_V3_HMAC_KEY=spec170-v3-network-diagnostic-key

cat >"$ROOT/bootstrap.tokens" <<'EOF'
# identity token role
/NDNSF-DI/Tracer/user user0001 user
/NDNSF-DI/Tracer/provider/backbone back0001 provider
/NDNSF-DI/Tracer/provider/head0 head0001 provider
/NDNSF-DI/Tracer/provider/head1 head1001 provider
/NDNSF-DI/Tracer/provider/merge merge001 provider
EOF

PIDS=()
cleanup() {
  rc=$?
  trap - EXIT INT TERM
  for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
  for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
  evidence="/evidence/spec170/d0-v3-workload-${SLURM_JOB_ID:-incontainer}"
  mkdir -p "$evidence"
  cp -a "$LOG" "$evidence/" 2>/dev/null || true
  cp -a "$STATUS" "$evidence/" 2>/dev/null || true
  cp -a "$ROOT/bootstrap.tokens" "$evidence/" 2>/dev/null || true
  printf 'SPEC170_D0_V3_WORKLOAD_TERMINAL exit=%s bundle=%s\n' "$rc" "$BUNDLE"
  exit "$rc"
}
trap cleanup EXIT INT TERM

nfd --config "$BUNDLE/nfd.conf" >"$LOG/nfd.log" 2>&1 & PIDS+=("$!")
nfd_ready=0
for _ in $(seq 1 100); do
  if nfdc status >"$STATUS/nfdc-status.txt" 2>&1; then
    nfd_ready=1
    break
  fi
  sleep 0.1
done
if [ "$nfd_ready" -ne 1 ]; then
  echo SPEC170_D0_V3_NFD_READY_FAIL
  exit 10
fi
nfdc strategy set /NDNSF-DI/Tracer/group \
  /localhost/nfd/strategy/multicast >/dev/null 2>&1 || true
nfdc strategy set /NDNSF-DI/Tracer \
  /localhost/nfd/strategy/multicast >/dev/null 2>&1 || true

App_ServiceController \
  --policy-file "$BUNDLE/controller.policies" \
  --trust-schema "$BUNDLE/trust-schema.conf" \
  --controller-prefix /NDNSF-DI/Tracer/controller \
  --bootstrap-token-file "$ROOT/bootstrap.tokens" \
  >"$LOG/controller.log" 2>&1 & PIDS+=("$!")

controller_ready=0
for _ in $(seq 1 100); do
  if ndnsec cert-dump -i /NDNSF-DI/Tracer/controller \
      >"$ROOT/controller.cert.tmp" 2>"$STATUS/controller-cert.err" \
      && test -s "$ROOT/controller.cert.tmp"; then
    mv "$ROOT/controller.cert.tmp" "$ROOT/controller.cert"
    controller_ready=1
    break
  fi
  sleep 0.1
done
if [ "$controller_ready" -ne 1 ]; then
  echo SPEC170_D0_V3_CONTROLLER_CERT_FAIL
  exit 10
fi

provider() {
  name="$1"
  provider_identity="$2"
  role="$3"
  token="$4"
  artifact="$5"
  python3 "$BUNDLE/spec170_v3_cpu_provider.py" \
    --provider "$provider_identity" --role "$role" \
    --group /NDNSF-DI/Tracer/group \
    --controller /NDNSF-DI/Tracer/controller \
    --trust-schema "$BUNDLE/trust-schema.conf" \
    --bootstrap-token "$token" \
    --onnx --artifact "$artifact" --device cpu \
    >"$LOG/$name.log" 2>&1
  printf '%s\n' "$?" >"$STATUS/$name.status"
}

provider backbone /NDNSF-DI/Tracer/provider/backbone /Backbone back0001 \
  "$BUNDLE/artifacts/qwen-native-tracer-backbone.onnx" & PIDS+=("$!")
provider head0 /NDNSF-DI/Tracer/provider/head0 /Head/Shard/0 head0001 \
  "$BUNDLE/artifacts/qwen-native-tracer-head0.onnx" & PIDS+=("$!")
provider head1 /NDNSF-DI/Tracer/provider/head1 /Head/Shard/1 head1001 \
  "$BUNDLE/artifacts/qwen-native-tracer-head1.onnx" & PIDS+=("$!")
provider merge /NDNSF-DI/Tracer/provider/merge /Merge merge001 \
  "$BUNDLE/artifacts/qwen-native-tracer-merge.onnx" & PIDS+=("$!")

providers_ready=0
for _ in $(seq 1 300); do
  count=0
  for name in backbone head0 head1 merge; do
    grep -q 'SPEC170_V3_PROVIDER_READY' "$LOG/$name.log" 2>/dev/null \
      && count=$((count + 1))
  done
  if [ "$count" -eq 4 ]; then
    providers_ready=1
    break
  fi
  sleep 0.1
done
if [ "$providers_ready" -ne 1 ]; then
  echo SPEC170_D0_V3_PROVIDER_READY_FAIL
  exit 11
fi

runtimes_ready=0
for _ in $(seq 1 100); do
  count=0
  for name in backbone head0 head1 merge; do
    grep -q 'SPEC170_V3_PROVIDER_RUNTIME_READY' "$LOG/$name.log" 2>/dev/null \
      && count=$((count + 1))
  done
  if [ "$count" -eq 4 ]; then
    runtimes_ready=1
    break
  fi
  sleep 0.1
done
if [ "$runtimes_ready" -ne 1 ]; then
  echo SPEC170_D0_V3_RUNTIME_READY_FAIL
  exit 11
fi

set +e
python3 "$BUNDLE/spec170_v3_cpu_user.py" \
  --group /NDNSF-DI/Tracer/group \
  --controller /NDNSF-DI/Tracer/controller \
  --user /NDNSF-DI/Tracer/user \
  --trust-schema "$BUNDLE/trust-schema.conf" \
  --bootstrap-token user0001 --device cpu \
  --ack-timeout-ms 1500 --timeout-ms 15000 \
  >"$LOG/user.log" 2>&1
user_rc=$?
set -u

# Merge publishes the final Response before the other selected Providers have
# necessarily flushed their local execution marker.  Drain for a bounded
# interval before the cleanup trap terminates the four processes.
execution_count=0
for _ in $(seq 1 50); do
  execution_count=0
  for name in backbone head0 head1 merge; do
    grep -q 'SPEC170_V3_PROVIDER_EXECUTION' "$LOG/$name.log" 2>/dev/null \
      && execution_count=$((execution_count + 1))
  done
  [ "$execution_count" -eq 4 ] && break
  sleep 0.1
done
echo "SPEC170_D0_V3_EXECUTION_DRAIN count=$execution_count"

for name in controller backbone head0 head1 merge user; do
  echo "=== $name ==="
  cat "$LOG/$name.log" 2>/dev/null || true
done

if [ "$user_rc" -eq 0 ] \
   && grep -q 'SPEC170_V3_USER_ACK_CLOSED' "$LOG/user.log" \
   && grep -q 'SPEC170_V3_USER_SELECTION_COMMITTED' "$LOG/user.log" \
   && grep -q 'SPEC170_V3_USER_RESPONSE' "$LOG/user.log" \
   && grep -q 'SPEC170_V3_PROVIDER_SELECTED' "$LOG/backbone.log" \
   && grep -q 'SPEC170_V3_PROVIDER_SELECTED' "$LOG/head0.log" \
   && grep -q 'SPEC170_V3_PROVIDER_SELECTED' "$LOG/head1.log" \
   && grep -q 'SPEC170_V3_PROVIDER_RESPONSE' "$LOG/merge.log" \
   && [ "$execution_count" -eq 4 ]; then
  echo "SPEC170_D0_V3_CPU_WORKLOAD_PASS job=${SLURM_JOB_ID:-incontainer}"
  exit 0
fi

echo "SPEC170_D0_V3_CPU_WORKLOAD_FAIL user_rc=$user_rc execution_count=$execution_count"
exit 12
