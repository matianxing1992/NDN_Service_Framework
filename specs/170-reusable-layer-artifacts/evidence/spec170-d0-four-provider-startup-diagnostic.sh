#!/usr/bin/env bash
# Diagnostic only: start NFD, Controller, and four CPU Providers with the
# exact Spec170 SIF/bundle. It does not run a user request and is not a gate
# pass.
set -u

BUNDLE="${1:?bundle path required}"
ROOT=/scratch
LOG="$ROOT/diagnostic-log"
KEYCHAINS="$ROOT/diagnostic-keychains"
mkdir -p "$ROOT/run" "$LOG"
for role in nfd nfdctl controller backbone head0 head1 merge; do
  mkdir -p "$KEYCHAINS/$role/pib" "$KEYCHAINS/$role/tpm"
done

keychain_env() {
  local role="$1"
  printf 'NDN_CLIENT_PIB=pib-sqlite3:%s NDN_CLIENT_TPM=tpm-file:%s' \
    "$KEYCHAINS/$role/pib" "$KEYCHAINS/$role/tpm"
}

sed -e 's|@@NFD_SOCKET@@|/scratch/run/nfd.sock|g' \
    -e 's|@@TCP_PORT@@|6363|g' \
    "$BUNDLE/nfd.conf.in" > "$ROOT/nfd.conf"
export NDN_CLIENT_TRANSPORT=unix:///scratch/run/nfd.sock
export NDNSF_CONTROLLER_CERT_FILE=/scratch/controller.cert
printf '%s\n' \
  '/NDNSF-DI/Tracer/user user0001 user' \
  '/NDNSF-DI/Tracer/provider/backbone back0001 provider' \
  '/NDNSF-DI/Tracer/provider/head0 head0001 provider' \
  '/NDNSF-DI/Tracer/provider/head1 head1001 provider' \
  '/NDNSF-DI/Tracer/provider/merge merge0001 provider' \
  > "$ROOT/bootstrap.tokens"

PIDS=()
cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  for pid in "${PIDS[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
  sleep 1
  for pid in "${PIDS[@]}"; do kill -KILL "$pid" 2>/dev/null || true; done
  for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
  printf 'SPEC170_D0_SINGLE_DIAGNOSTIC_TERMINAL exit=%s\n' "$rc"
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
      >"$LOG/nfdc-status.txt" 2>&1; then
    nfd_ready=1
    break
  fi
  sleep 0.1
done
if [[ "$nfd_ready" -ne 1 ]]; then
  echo SPEC170_D0_SINGLE_NFD_READY_FAIL
  exit 10
fi

env "$controller_pib" "$controller_tpm" App_ServiceController \
  --policy-file "$BUNDLE/controller.policies" \
  --trust-schema "$BUNDLE/trust-schema.conf" \
  --controller-prefix /NDNSF-DI/Tracer/controller \
  --bootstrap-token-file "$ROOT/bootstrap.tokens" \
  >"$LOG/controller.log" 2>&1 & PIDS+=("$!")
controller_pid="${PIDS[$(( ${#PIDS[@]} - 1 ))]}"
for _ in $(seq 1 1200); do
  if grep -q 'ServiceController started...' "$LOG/controller.log" 2>/dev/null; then
    break
  fi
  if ! kill -0 "$controller_pid" 2>/dev/null; then
    echo SPEC170_D0_SINGLE_CONTROLLER_EXITED
    break
  fi
  sleep 0.1
done

controller_ready=0
for _ in $(seq 1 1200); do
  if env "$controller_pib" "$controller_tpm" ndnsec cert-dump \
      -i /NDNSF-DI/Tracer/controller >"$ROOT/controller.cert.tmp" \
      2>"$LOG/controller-cert.err" && test -s "$ROOT/controller.cert.tmp"; then
    mv "$ROOT/controller.cert.tmp" "$ROOT/controller.cert"
    controller_ready=1
    break
  fi
  sleep 0.1
done
if [[ "$controller_ready" -ne 1 ]]; then
  echo SPEC170_D0_SINGLE_CONTROLLER_CERT_FAIL
  exit 11
fi

declare -a provider_names=(backbone head0 head1 merge)
declare -a provider_ids=(
  /NDNSF-DI/Tracer/provider/backbone
  /NDNSF-DI/Tracer/provider/head0
  /NDNSF-DI/Tracer/provider/head1
  /NDNSF-DI/Tracer/provider/merge)
declare -a provider_roles=(/Backbone /Head/Shard/0 /Head/Shard/1 /Merge)
declare -a provider_tokens=(back0001 head0001 head1001 merge0001)
for index in "${!provider_names[@]}"; do
  name="${provider_names[$index]}"
  read -r provider_pib provider_tpm < <(keychain_env "$name")
  (
    cd "$BUNDLE"
    env "$provider_pib" "$provider_tpm" \
      /opt/ndnsf-di/current/bin/di-native-provider \
      --plan "$BUNDLE/native-execution-plan.json" \
      --manifest "$BUNDLE/service-manifest.json" \
      --service /Inference/NativeTracer \
      --provider "${provider_ids[$index]}" --roles "${provider_roles[$index]}" \
      --workers 1 --handler-threads 2 --ack-threads 2 \
      --group /NDNSF-DI/Tracer/group \
      --controller /NDNSF-DI/Tracer/controller \
      --trust-schema "$BUNDLE/trust-schema.conf" \
      --bootstrap-token "${provider_tokens[$index]}" \
      --artifact-cache-dir "$ROOT/cache-$name" \
      --serve
  ) >"$LOG/$name.log" 2>&1 & PIDS+=("$!")
done

for _ in $(seq 1 1800); do
  ready=0
  alive=0
  for name in "${provider_names[@]}"; do
    grep -q 'NDNSF_DI_NATIVE_PROVIDER_READY' "$LOG/$name.log" 2>/dev/null \
      && ready=$((ready + 1))
    pid_index=-1
    case "$name" in
      backbone) pid_index=2;;
      head0) pid_index=3;;
      head1) pid_index=4;;
      merge) pid_index=5;;
    esac
    if kill -0 "${PIDS[$pid_index]}" 2>/dev/null; then
      alive=$((alive + 1))
    fi
  done
  if [[ "$ready" -eq 4 ]]; then
    echo SPEC170_D0_FOUR_PROVIDER_READY
    break
  fi
  if [[ "$alive" -lt 4 ]]; then
    echo "SPEC170_D0_FOUR_PROVIDER_EARLY_EXIT ready=$ready alive=$alive"
    break
  fi
  sleep 0.1
done

for file in "$LOG"/*; do
  echo "=== $file"
  sed -n '1,220p' "$file"
done
