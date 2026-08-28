#!/usr/bin/env bash
set -u

rank=${NDNSF_D2H_RANK:-}
run_id=${NDNSF_D2H_RUN_ID:-}
peer_host=${NDNSF_D2H_PEER_HOST:-}
mapping=${SPEC170_D2H_MAPPING:-121}
device=${SPEC170_D2H_DEVICE:-cuda:0}
if [[ "$rank" != 0 && "$rank" != 1 ]] || [[ -z "$run_id" ]] || [[ -z "$peer_host" ]]; then
  echo "SPEC170_D2H_ENV_FAIL rank=$rank runId=$run_id peer=$peer_host" >&2; exit 2
fi
if [[ "$mapping" != 121 && "$mapping" != 212 ]]; then
  echo "SPEC170_D2H_MAPPING_FAIL mapping=$mapping" >&2; exit 2
fi

ROOT=/scratch
LOG="$ROOT/log"
STATUS="$ROOT/status"
KEYCHAIN_ROOT="$ROOT/keychains"
COORD="/evidence/spec170/d2h-hybrid-$run_id-$mapping"
mkdir -p "$ROOT/run" "$LOG" "$STATUS" "$KEYCHAIN_ROOT" "$COORD"
export SPEC170_D2H_EVIDENCE_DIR="$COORD"
export NDN_CLIENT_TRANSPORT=unix:///scratch/run/nfd.sock

prepare_keychain() { mkdir -p "$KEYCHAIN_ROOT/$1/pib" "$KEYCHAIN_ROOT/$1/tpm"; }
keychain_env() { printf 'NDN_CLIENT_PIB=pib-sqlite3:%s NDN_CLIENT_TPM=tpm-file:%s' "$KEYCHAIN_ROOT/$1/pib" "$KEYCHAIN_ROOT/$1/tpm"; }
for role in nfd nfdctl controller provider user; do prepare_keychain "$role"; done

BUNDLE=$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd) || {
  echo SPEC170_D2H_BUNDLE_RESOLVE_FAIL >&2
  exit 20
}
for required in nfd.conf controller.policies trust-schema.conf \
  spec170_v3_hybrid_provider.py spec170_v3_hybrid_user.py; do
  test -f "$BUNDLE/$required" || {
    echo "SPEC170_D2H_BUNDLE_FAIL missing=$BUNDLE/$required" >&2
    exit 20
  }
done
artifact_root="$BUNDLE/artifacts"
test -d "$artifact_root" || {
  echo "SPEC170_D2H_ARTIFACT_ROOT_FAIL path=$artifact_root" >&2
  exit 20
}
cd "$BUNDLE"
TCP_PORT=$(sed -n '/^[[:space:]]*tcp[[:space:]]*$/,/^[[:space:]]*}/p' "$BUNDLE/nfd.conf" | awk '/port[[:space:]]+[0-9]+/ {print $2; exit}')
if ! [[ "$TCP_PORT" =~ ^[0-9]+$ ]]; then echo "SPEC170_D2H_TCP_PORT_FAIL value=$TCP_PORT" >&2; exit 21; fi

cat >"$ROOT/bootstrap.tokens" <<'EOF'
# identity token role
/NDNSF-DI/Tracer/user user0001 user
/NDNSF-DI/Tracer/provider/p0 p0001 provider
/NDNSF-DI/Tracer/provider/p1 p1001 provider
EOF

PIDS=()
cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
  for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
  mkdir -p "$COORD/rank-$rank"
  cp -a "$LOG" "$COORD/rank-$rank/" 2>/dev/null || true
  cp -a "$STATUS" "$COORD/rank-$rank/" 2>/dev/null || true
  printf 'rank=%s\nrunId=%s\npeerHost=%s\ntcpPort=%s\nmapping=%s\n' "$rank" "$run_id" "$peer_host" "$TCP_PORT" "$mapping" >"$COORD/rank-$rank/manifest.txt"
  echo "SPEC170_D2H_WORKLOAD_TERMINAL rank=$rank exit=$rc runId=$run_id mapping=$mapping"
  exit "$rc"
}
trap cleanup EXIT INT TERM

read -r nfd_pib nfd_tpm < <(keychain_env nfd)
read -r nfdctl_pib nfdctl_tpm < <(keychain_env nfdctl)
env "$nfd_pib" "$nfd_tpm" nfd --config "$BUNDLE/nfd.conf" >"$LOG/nfd.log" 2>&1 &
nfd_pid=$!
PIDS+=( "$nfd_pid" )
nfd_start_ms=$(date +%s%3N)
nfd_ready=0
for _ in $(seq 1 600); do
  if env "$nfdctl_pib" "$nfdctl_tpm" nfdc status >"$STATUS/nfdc-status.txt" 2>&1; then nfd_ready=1; break; fi
  if ! kill -0 "$nfd_pid" 2>/dev/null; then
    wait "$nfd_pid"; nfd_rc=$?
    echo "SPEC170_D2H_NFD_EXITED rc=$nfd_rc" | tee "$STATUS/nfd-start.txt"
    exit 10
  fi
  sleep 0.1
done
printf 'ready=%s elapsedMs=%s pid=%s\n' "$nfd_ready" \
  "$(( $(date +%s%3N) - nfd_start_ms ))" "$nfd_pid" >"$STATUS/nfd-start.txt"
if [[ "$nfd_ready" -ne 1 ]]; then echo SPEC170_D2H_NFD_READY_FAIL; exit 10; fi

peer_uri="tcp4://$peer_host:$TCP_PORT"
peer_face=""
for _ in $(seq 1 1200); do
  if env "$nfdctl_pib" "$nfdctl_tpm" nfdc face create "$peer_uri" >"$STATUS/face-create.txt" 2>&1; then peer_face="$peer_uri"; break; fi
  sleep 0.2
done
if [[ -z "$peer_face" ]]; then echo SPEC170_D2H_PEER_FACE_FAIL; exit 11; fi
env "$nfdctl_pib" "$nfdctl_tpm" nfdc route add /NDNSF-DI/Tracer "$peer_face" origin 65 cost 0 >"$STATUS/route-add.txt" 2>&1 || { echo SPEC170_D2H_ROUTE_FAIL; exit 12; }
env "$nfdctl_pib" "$nfdctl_tpm" nfdc strategy set /NDNSF-DI/Tracer/group /localhost/nfd/strategy/multicast >"$STATUS/strategy-set.txt" 2>&1 || { echo SPEC170_D2H_STRATEGY_FAIL; exit 12; }
printf 'peer=%s port=%s face=%s mapping=%s\n' "$peer_host" "$TCP_PORT" "$peer_face" "$mapping" >"$STATUS/peer-route.txt"

if [[ "$rank" -eq 0 ]]; then
  read -r controller_pib controller_tpm < <(keychain_env controller)
  policy_file="$BUNDLE/controller.policies"
  env "$controller_pib" "$controller_tpm" App_ServiceController --policy-file "$policy_file" --trust-schema "$BUNDLE/trust-schema.conf" --controller-prefix /NDNSF-DI/Tracer/controller --bootstrap-token-file "$ROOT/bootstrap.tokens" >"$LOG/controller.log" 2>&1 & controller_pid=$!; PIDS+=( "$controller_pid" )
  controller_started=0
  for _ in $(seq 1 1200); do
    if grep -q 'ServiceController started...' "$LOG/controller.log" 2>/dev/null; then
      controller_started=1; break
    fi
    kill -0 "$controller_pid" 2>/dev/null || break
    sleep 0.1
  done
  if [[ "$controller_started" -ne 1 ]]; then echo SPEC170_D2H_CONTROLLER_START_FAIL; exit 13; fi
  controller_ready=0
  for _ in $(seq 1 600); do
    if env "$controller_pib" "$controller_tpm" ndnsec cert-dump -i /NDNSF-DI/Tracer/controller >"$ROOT/controller.cert.tmp" 2>"$STATUS/controller-cert.err" && [[ -s "$ROOT/controller.cert.tmp" ]]; then
      mv "$ROOT/controller.cert.tmp" "$ROOT/controller.cert"; cp "$ROOT/controller.cert" "$COORD/controller.cert"; controller_ready=1; break
    fi
    sleep 0.1
  done
  if [[ "$controller_ready" -ne 1 ]]; then echo SPEC170_D2H_CONTROLLER_CERT_FAIL; exit 13; fi
else
  for _ in $(seq 1 2400); do
    if [[ -s "$COORD/controller.cert" ]]; then cp "$COORD/controller.cert" "$ROOT/controller.cert"; break; fi
    sleep 0.1
  done
  [[ -s "$ROOT/controller.cert" ]] || { echo SPEC170_D2H_CONTROLLER_WAIT_FAIL; exit 14; }
fi
export NDNSF_CONTROLLER_CERT_FILE=/scratch/controller.cert

if [[ "$mapping" == 121 ]]; then
  if [[ "$rank" -eq 0 ]]; then provider_roles="/Pipeline/S0/R0,/Pipeline/S1/R0"; else provider_roles="/Pipeline/S1/R1,/Pipeline/S2/R0"; fi
else
  if [[ "$rank" -eq 0 ]]; then provider_roles="/Pipeline/S0/R0,/Pipeline/S1/R0,/Pipeline/S2/R0"; else provider_roles="/Pipeline/S0/R1,/Pipeline/S2/R1"; fi
fi
provider_identity="/NDNSF-DI/Tracer/provider/p$rank"
provider_token=$([[ "$rank" -eq 0 ]] && echo p0001 || echo p1001)
read -r provider_pib provider_tpm < <(keychain_env provider)
provider_args=( --provider "$provider_identity" --peer "/NDNSF-DI/Tracer/provider/p$((1-rank))" --roles "$provider_roles" --group /NDNSF-DI/Tracer/group --controller /NDNSF-DI/Tracer/controller --trust-schema "$BUNDLE/trust-schema.conf" --bootstrap-token "$provider_token" --mapping "$mapping" --artifact-root "$artifact_root" --device "$device" )
printf 'SPEC170_D2H_PROVIDER_START provider=%s rank=%s device=%s mapping=%s\n' \
  "$provider_identity" "$rank" "$device" "$mapping" >"$LOG/provider.log"
env "$provider_pib" "$provider_tpm" /opt/venv/bin/python -u "$BUNDLE/spec170_v3_hybrid_provider.py" "${provider_args[@]}" >>"$LOG/provider.log" 2>&1 & provider_pid=$!; PIDS+=( "$provider_pid" )
# Cold exact-SIF Python/CUDA startup can take several minutes on shared
# storage; keep it bounded without turning startup latency into a feature fail.
for _ in $(seq 1 9000); do
  grep -q SPEC170_D2H_PROVIDER_READY "$LOG/provider.log" 2>/dev/null && { : >"$COORD/provider-$rank.ready"; break; }
  kill -0 "$provider_pid" 2>/dev/null || { echo SPEC170_D2H_PROVIDER_EXITED; exit 15; }
  sleep 0.1
done
[[ -f "$COORD/provider-$rank.ready" ]] || { echo SPEC170_D2H_PROVIDER_READY_FAIL; exit 16; }

if [[ "$rank" -eq 0 ]]; then
  for _ in $(seq 1 9000); do [[ -f "$COORD/provider-0.ready" && -f "$COORD/provider-1.ready" ]] && break; sleep 0.1; done
  [[ -f "$COORD/provider-1.ready" ]] || { echo SPEC170_D2H_PEER_PROVIDER_READY_FAIL; exit 17; }
  read -r user_pib user_tpm < <(keychain_env user)
  run_user_case() {
    local case_name=$1
    local output="$LOG/user-${case_name}.log"
    env "$user_pib" "$user_tpm" /opt/venv/bin/python "$BUNDLE/spec170_v3_hybrid_user.py" --mapping "$mapping" --artifact-root "$artifact_root" --group /NDNSF-DI/Tracer/group --controller /NDNSF-DI/Tracer/controller --user /NDNSF-DI/Tracer/user --trust-schema "$BUNDLE/trust-schema.conf" --bootstrap-token user0001 --provider0 /NDNSF-DI/Tracer/provider/p0 --provider1 /NDNSF-DI/Tracer/provider/p1 --device "$device" --case "$case_name" --request-id "spec170-v3-hybrid-${mapping}-${case_name}" --ack-timeout-ms 2500 --timeout-ms 30000 >"$output" 2>&1
  }
  run_user_case positive; user_rc=$?
  [[ "$user_rc" -eq 0 ]] || { echo SPEC170_D2H_USER_FAIL case=positive rc=$user_rc; exit 18; }
  grep -q SPEC170_D2H_USER_RESPONSE "$LOG/user-positive.log" || { echo SPEC170_D2H_USER_RESPONSE_MISSING; exit 19; }
  run_user_case missing; user_rc=$?
  [[ "$user_rc" -eq 0 ]] || { echo SPEC170_D2H_NEGATIVE_USER_FAIL rc=$user_rc; exit 20; }
  grep -q "SPEC170_D2H_NEGATIVE_PASS case=missing" "$LOG/user-missing.log" || { echo SPEC170_D2H_NEGATIVE_MARKER_MISSING; exit 21; }
  echo "SPEC170_D2H_HYBRID_WORKLOAD_PASS runId=$run_id mapping=$mapping negatives=1"
else
  for _ in $(seq 1 9000); do [[ -f "$COORD/rank-0/log/user-positive.log" ]] && break; sleep 0.1; done
fi
exit 0
