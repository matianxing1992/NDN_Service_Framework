#!/usr/bin/env bash
set -u

rank=${NDNSF_D2B_RANK:-}
run_id=${NDNSF_D2B_RUN_ID:-}
peer_host=${NDNSF_D2B_PEER_HOST:-}
case_name=${NDNSF_D2B_CASE:-positive}
device=${SPEC170_D2B_DEVICE:-cuda:0}
if [[ "$rank" != 0 && "$rank" != 1 ]] || [[ -z "$run_id" ]] || [[ -z "$peer_host" ]]; then
  echo "SPEC170_D2B_ENV_FAIL rank=$rank runId=$run_id peer=$peer_host" >&2
  exit 2
fi
case "$case_name" in
  positive|peer-mismatch|replay|partial) ;;
  *) echo "SPEC170_D2B_CASE_FAIL case=$case_name" >&2; exit 2 ;;
esac

ROOT=/scratch
LOG="$ROOT/log"
STATUS="$ROOT/status"
KEYCHAIN_ROOT="$ROOT/keychains"
COORD="/evidence/spec170/d2b-cross-provider-$run_id-$case_name"
mkdir -p "$ROOT/run" "$LOG" "$STATUS" "$KEYCHAIN_ROOT" "$COORD"
export SPEC170_D2B_EVIDENCE_DIR="$COORD"
export NDN_CLIENT_TRANSPORT=unix:///scratch/run/nfd.sock
# Keep the explicit barrier diagnostic: the provider/user control path must
# leave enough evidence to distinguish a missing ACK from a stale policy or
# a forwarding failure.  The default remains quiet outside this workload.
export NDN_LOG="${NDN_LOG:-ndn_service_framework.*=TRACE}"
export NDNSF_DI_TRACE_USER_STAGES=1

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

BUNDLE=$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd) || {
  echo SPEC170_D2B_BUNDLE_RESOLVE_FAIL >&2
  exit 20
}
for required in nfd.conf controller.policies trust-schema.conf \
  native-execution-plan.json service-manifest.json assignment.csv user_driver.py \
  artifacts/qwen-native-tracer-backbone.onnx \
  artifacts/qwen-native-tracer-head0.onnx; do
  test -f "$BUNDLE/$required" || {
    echo "SPEC170_D2B_BUNDLE_FAIL missing=$BUNDLE/$required" >&2
    exit 20
  }
done
cd "$BUNDLE"
TCP_PORT=$(awk '
  /^[[:space:]]*tcp[[:space:]]*$/ { in_tcp=1; next }
  in_tcp && /^[[:space:]]*\{/ { next }
  in_tcp && /^[[:space:]]*port[[:space:]]+[0-9]+/ { print $2; exit }
  in_tcp && /^[[:space:]]*\}/ { in_tcp=0 }
' "$BUNDLE/nfd.conf")
if ! [[ "$TCP_PORT" =~ ^[0-9]+$ ]]; then
  echo "SPEC170_D2B_TCP_PORT_FAIL value=$TCP_PORT"
  exit 21
fi

cat >"$ROOT/bootstrap.tokens" <<'EOF'
# identity token role
/NDNSF-DI/Tracer/user user0001 user
/NDNSF-DI/Tracer/provider/rank0 rank0001 provider
/NDNSF-DI/Tracer/provider/rank1 rank1001 provider
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
  printf 'rank=%s\nrunId=%s\ncase=%s\npeerHost=%s\ntcpPort=%s\n' \
    "$rank" "$run_id" "$case_name" "$peer_host" "$TCP_PORT" >"$COORD/rank-$rank/manifest.txt"
  echo "SPEC170_D2B_WORKLOAD_TERMINAL rank=$rank exit=$rc runId=$run_id case=$case_name"
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
  if env "$nfdctl_pib" "$nfdctl_tpm" nfdc status >"$STATUS/nfdc-status.txt" 2>&1; then
    nfd_ready=1; break
  fi
  if ! kill -0 "$nfd_pid" 2>/dev/null; then
    wait "$nfd_pid"; nfd_rc=$?
    echo "SPEC170_D2B_NFD_EXITED rc=$nfd_rc" | tee "$STATUS/nfd-start.txt"
    exit 10
  fi
  sleep 0.1
done
printf 'ready=%s elapsedMs=%s pid=%s\n' "$nfd_ready" \
  "$(( $(date +%s%3N) - nfd_start_ms ))" "$nfd_pid" >"$STATUS/nfd-start.txt"
if [[ "$nfd_ready" -ne 1 ]]; then echo SPEC170_D2B_NFD_READY_FAIL; exit 10; fi

peer_uri="tcp4://$peer_host:$TCP_PORT"
# A cold exact-SIF --nv start on some Tiger nodes can take tens of seconds.
# Keep peer-face establishment bounded, but do not let the faster rank exit
# while the other rank is still materializing the image and starting NFD.
peer_face=""
for _ in $(seq 1 900); do
  if env "$nfdctl_pib" "$nfdctl_tpm" nfdc face create "$peer_uri" \
      >"$STATUS/face-create.txt" 2>&1; then
    # nfdc route add accepts a face URI and resolves the current face ID.
    # Avoid parsing version-specific face-list/face-create text output.
    peer_face="$peer_uri"
    break
  fi
  sleep 0.2
done
if [[ -z "$peer_face" ]]; then echo SPEC170_D2B_PEER_FACE_FAIL; exit 11; fi
env "$nfdctl_pib" "$nfdctl_tpm" nfdc route add /NDNSF-DI/Tracer "$peer_face" origin 65 cost 0 >"$STATUS/route-add.txt" 2>&1 ||
  { echo SPEC170_D2B_ROUTE_FAIL; exit 12; }
# The local and peer Providers share the SVS group prefix.  Without multicast,
# NFD's default best-route strategy can select only the local multicast face;
# install the same strategy on both ranks so sync Interests reach the peer.
env "$nfdctl_pib" "$nfdctl_tpm" nfdc strategy set /NDNSF-DI/Tracer/group \
  /localhost/nfd/strategy/multicast >"$STATUS/strategy-set.txt" 2>&1 ||
  { echo SPEC170_D2B_STRATEGY_FAIL; exit 12; }
printf 'peer=%s port=%s face=%s\n' "$peer_host" "$TCP_PORT" "$peer_face" >"$STATUS/peer-route.txt"

if [[ "$rank" -eq 0 ]]; then
  read -r controller_pib controller_tpm < <(keychain_env controller)
  env "$controller_pib" "$controller_tpm" App_ServiceController \
    --policy-file "$BUNDLE/controller.policies" \
    --trust-schema "$BUNDLE/trust-schema.conf" \
    --controller-prefix /NDNSF-DI/Tracer/controller \
    --bootstrap-token-file "$ROOT/bootstrap.tokens" \
    >"$LOG/controller.log" 2>&1 &
  controller_pid=$!
  PIDS+=( "$controller_pid" )
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
    echo SPEC170_D2B_CONTROLLER_START_FAIL
    exit 13
  fi
  controller_ready=0
  for _ in $(seq 1 150); do
    if env "$controller_pib" "$controller_tpm" ndnsec cert-dump -i /NDNSF-DI/Tracer/controller \
        >"$ROOT/controller.cert.tmp" 2>"$STATUS/controller-cert.err" && [[ -s "$ROOT/controller.cert.tmp" ]]; then
      mv "$ROOT/controller.cert.tmp" "$ROOT/controller.cert"
      cp "$ROOT/controller.cert" "$COORD/controller.cert"
      controller_ready=1; break
    fi
    sleep 0.1
  done
  if [[ "$controller_ready" -ne 1 ]]; then echo SPEC170_D2B_CONTROLLER_CERT_FAIL; exit 13; fi
else
  # The first rank can take several minutes to materialize the SIF and create
  # the controller certificate on a cold shared-filesystem node.  Keep this
  # bounded, but do not discard a healthy peer because a 30 s barrier expired.
  for _ in $(seq 1 1800); do
    if [[ -s "$COORD/controller.cert" ]]; then cp "$COORD/controller.cert" "$ROOT/controller.cert"; break; fi
    sleep 0.1
  done
  [[ -s "$ROOT/controller.cert" ]] || { echo SPEC170_D2B_CONTROLLER_WAIT_FAIL; exit 14; }
fi
export NDNSF_CONTROLLER_CERT_FILE=/scratch/controller.cert

provider_identity="/NDNSF-DI/Tracer/provider/rank$rank"
if [[ "$rank" -eq 0 ]]; then
  provider_role="/Backbone"
else
  provider_role="/Head/Shard/0"
fi
provider_token=$([[ "$rank" -eq 0 ]] && echo rank0001 || echo rank1001)
read -r provider_pib provider_tpm < <(keychain_env provider)
provider_args=(
  --plan "$BUNDLE/native-execution-plan.json"
  --manifest "$BUNDLE/service-manifest.json"
  --service /Inference/Spec170Collective
  --provider "$provider_identity"
  --roles "$provider_role"
  --group /NDNSF-DI/Tracer/group
  --controller /NDNSF-DI/Tracer/controller
  --trust-schema "$BUNDLE/trust-schema.conf"
  --bootstrap-token "$provider_token"
  --workers 1
  --handler-threads 2
  --ack-threads 2
  --execution-policy DATA_DRIVEN_V2
  --serve
)
if [[ "$device" != cuda:0 ]]; then
  echo "SPEC170_D2B_DEVICE_CONTRACT_FAIL expected=cuda:0 actual=$device"
  exit 15
fi
provider_bin=/opt/ndnsf-di/current/bin/di-native-provider
provider_fault_args=()
if [[ "$case_name" == partial && "$rank" -eq 1 ]]; then
  provider_bin=/opt/ndnsf-di/current/bin/di-native-fault-provider
  provider_fault_args=(
    --fault-type missing-segment
    --fault-role /Head/Shard/0
  )
fi
env "$provider_pib" "$provider_tpm" \
  "$provider_bin" "${provider_fault_args[@]}" \
  "${provider_args[@]}" \
  >"$LOG/provider.log" 2>&1 &
provider_pid=$!
PIDS+=( "$provider_pid" )
# Provider Python/ONNX initialization can be slower on a cold node than NFD;
# keep the readiness bound explicit but large enough for the first launch.
for _ in $(seq 1 1200); do
  # The native provider emits READY after model/runtime materialization, while
  # permission installation is asynchronous.  Use its explicit runtime
  # permission marker rather than parsing an optional NDN logger line; logger
  # verbosity is not a protocol readiness signal.
  if grep -q NDNSF_DI_NATIVE_PROVIDER_READY "$LOG/provider.log" 2>/dev/null && \
     grep -q NDNSF_DI_NATIVE_PROVIDER_PERMISSION_READY \
       "$LOG/provider.log" 2>/dev/null; then
    : >"$COORD/provider-$rank.ready"
    : >"$COORD/provider-$rank.permission-ready"
    break
  fi
  kill -0 "$provider_pid" 2>/dev/null || { echo SPEC170_D2B_PROVIDER_EXITED; exit 15; }
  sleep 0.1
done
[[ -f "$COORD/provider-$rank.ready" && -f "$COORD/provider-$rank.permission-ready" ]] || {
  echo SPEC170_D2B_PROVIDER_PERMISSION_READY_FAIL
  exit 16
}

if [[ "$case_name" == partial && "$rank" -eq 1 ]]; then
  (
    for _ in $(seq 1 1800); do
      if grep -q 'NDNSF_DI_EXPERIMENT_FAULT_INJECTED.*type=missing-segment' \
          "$LOG/provider.log" 2>/dev/null; then
        : >"$COORD/fault-missing-segment.injected"
        exit 0
      fi
      kill -0 "$provider_pid" 2>/dev/null || exit 0
      sleep 0.1
    done
  ) &
  PIDS+=( "$!" )
fi

if [[ "$rank" -eq 0 ]]; then
  # CUDA/ONNX Runtime initialization can exceed 30 seconds on a fresh node.
  # Keep the wait bounded, but do not race the second Provider's readiness.
  for _ in $(seq 1 1800); do
    [[ -f "$COORD/provider-0.ready" && -f "$COORD/provider-1.ready" ]] && break
    sleep 0.1
  done
  [[ -f "$COORD/provider-1.ready" ]] || { echo SPEC170_D2B_PEER_PROVIDER_READY_FAIL; exit 17; }
  read -r user_pib user_tpm < <(keychain_env user)
  run_user_case() {
    local user_case=$1
    local assignment_csv=${2:-$BUNDLE/assignment.csv}
    local fixed_request_id=${3:-}
    local output="$LOG/user-${user_case}.log"
    local ack_timeout_ms=1500
    # The missing-segment fault is injected after dependency fetch.  Give the
    # faulting Provider enough ACK time to enter the selected execution set;
    # otherwise the negative case only measures an early ACK_CLOSED race.
    [[ "$case_name" == partial ]] && ack_timeout_ms=15000
    local fixed_args=()
    if [[ -n "$fixed_request_id" ]]; then
      fixed_args=(--fixed-request-id "$fixed_request_id")
    fi
    env "$user_pib" "$user_tpm" /opt/venv/bin/python \
      "$BUNDLE/user_driver.py" \
      --plan "$BUNDLE/native-execution-plan.json" \
      --service /Inference/Spec170Collective \
      --group /NDNSF-DI/Tracer/group \
      --controller /NDNSF-DI/Tracer/controller \
      --user /NDNSF-DI/Tracer/user \
      --trust-schema "$BUNDLE/trust-schema.conf" \
      --bootstrap-token user0001 \
      --assignment-csv "$assignment_csv" \
      --ack-timeout-ms "$ack_timeout_ms" --timeout-ms 30000 \
      --requests 1 --concurrency 1 "${fixed_args[@]}" >"$output" 2>&1
  }
  case "$case_name" in
    positive)
      run_user_case positive
      user_rc=$?
      [[ "$user_rc" -eq 0 ]] || { echo SPEC170_D2B_USER_FAIL case=positive rc=$user_rc; exit 18; }
      grep -q 'NDNSF_DI_NATIVE_TRACER_USER_EXECUTION.*"status": "executed"' \
          "$LOG/user-positive.log" || {
        echo SPEC170_D2B_USER_RESPONSE_MISSING
        exit 19
      }
      : >"$COORD/d2b-positive.done"
      echo "SPEC170_D2B_POSITIVE_PASS runId=$run_id device=$device transport=NDNSF_DATA_V1"
      ;;
    peer-mismatch)
      mismatch_csv="$ROOT/assignment-peer-mismatch.csv"
      awk '
        NR == 1 { print; next }
        {
          gsub("__RANK0__", "/NDNSF-DI/Tracer/provider/rank1")
          gsub("__RANK1__", "/NDNSF-DI/Tracer/provider/rank0")
          print
        }
      ' <(sed -e 's@/NDNSF-DI/Tracer/provider/rank0@__RANK0__@' \
             -e 's@/NDNSF-DI/Tracer/provider/rank1@__RANK1__@' \
             "$BUNDLE/assignment.csv") >"$mismatch_csv"
      if run_user_case peer-mismatch "$mismatch_csv"; then
        echo SPEC170_D2B_NEGATIVE_UNEXPECTED_SUCCESS case=peer-mismatch
        exit 22
      fi
      if grep -q 'NDNSF_DI_NATIVE_TRACER_USER_EXECUTION.*"status": "executed"' \
          "$LOG/user-peer-mismatch.log"; then
        echo SPEC170_D2B_NEGATIVE_PARTIAL_RESPONSE case=peer-mismatch
        exit 23
      fi
      : >"$COORD/negative-peer-mismatch.pass"
      echo "SPEC170_D2B_NEGATIVE_PASS case=peer-mismatch runId=$run_id"
      ;;
    replay)
      replay_request_id="/spec170-d2b-replay-${run_id}"
      run_user_case replay-first "$BUNDLE/assignment.csv" "$replay_request_id" || {
        echo SPEC170_D2B_REPLAY_SEED_FAIL
        exit 24
      }
      grep -q 'NDNSF_DI_NATIVE_TRACER_USER_EXECUTION.*"status": "executed"' \
          "$LOG/user-replay-first.log" || {
        echo SPEC170_D2B_REPLAY_SEED_RESPONSE_MISSING
        exit 25
      }
      if run_user_case replay-second "$BUNDLE/assignment.csv" "$replay_request_id"; then
        echo SPEC170_D2B_NEGATIVE_UNEXPECTED_SUCCESS case=replay
        exit 26
      fi
      if grep -q 'NDNSF_DI_NATIVE_TRACER_USER_EXECUTION.*"status": "executed"' \
          "$LOG/user-replay-second.log"; then
        echo SPEC170_D2B_NEGATIVE_PARTIAL_RESPONSE case=replay
        exit 27
      fi
      : >"$COORD/negative-replay.pass"
      echo "SPEC170_D2B_NEGATIVE_PASS case=replay runId=$run_id requestId=$replay_request_id"
      ;;
    partial)
      if run_user_case partial; then
        echo SPEC170_D2B_NEGATIVE_UNEXPECTED_SUCCESS case=partial
        exit 28
      fi
      if grep -q 'NDNSF_DI_NATIVE_TRACER_USER_EXECUTION.*"status": "executed"' \
          "$LOG/user-partial.log"; then
        echo SPEC170_D2B_NEGATIVE_PARTIAL_RESPONSE case=partial
        exit 29
      fi
      for _ in $(seq 1 1800); do
        [[ -f "$COORD/fault-missing-segment.injected" ]] && break
        sleep 0.1
      done
      [[ -f "$COORD/fault-missing-segment.injected" ]] || {
        echo SPEC170_D2B_FAULT_MARKER_MISSING
        exit 30
      }
      : >"$COORD/negative-partial.pass"
      echo "SPEC170_D2B_NEGATIVE_PASS case=partial runId=$run_id"
      ;;
  esac
else
  for _ in $(seq 1 1800); do
    [[ -f "$COORD/d2b-positive.done" || -f "$COORD/negative-peer-mismatch.pass" || \
        -f "$COORD/negative-replay.pass" || -f "$COORD/negative-partial.pass" ]] && break
    sleep 0.1
  done
  if [[ ! -f "$COORD/d2b-positive.done" && ! -f "$COORD/negative-peer-mismatch.pass" && \
        ! -f "$COORD/negative-replay.pass" && ! -f "$COORD/negative-partial.pass" ]]; then
    echo SPEC170_D2B_CASE_WAIT_FAIL case=$case_name
    exit 20
  fi
fi
exit 0
