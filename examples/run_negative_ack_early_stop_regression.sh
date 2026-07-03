#!/usr/bin/env bash
set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmpdir="$(mktemp -d /tmp/ndnsf-negative-ack.XXXXXX)"

controller_pid=""
provider_a_pid=""
provider_b_pid=""
nfd_started="false"

cleanup() {
  for pid in "${provider_a_pid}" "${provider_b_pid}" "${controller_pid}"; do
    if [[ -n "${pid}" ]]; then
      kill "${pid}" 2>/dev/null || true
      wait "${pid}" 2>/dev/null || true
    fi
  done
  if [[ "${nfd_started}" == "true" ]]; then
    nfd-stop >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

cd "${repo_root}"
export LD_LIBRARY_PATH="${repo_root}/build:${LD_LIBRARY_PATH:-}"
export NDNSF_DISABLE_NDNSD=1
export NDNSF_CONFIG="${tmpdir}/ndnsf.conf"
export NDNSF_SESSION_BASE="$(( $(date +%s) + $$ ))"
export NDN_LOG="${NDN_LOG:-ndn_service_framework.*=TRACE}"

if ! pgrep -x nfd >/dev/null 2>&1; then
  nfd-start >"${tmpdir}/nfd.log" 2>&1
  nfd_started="true"
  sleep 2
fi

nfdc strategy set /example/hello/group /localhost/nfd/strategy/multicast/v=5 >/dev/null 2>&1 || true

wait_for_controller() {
  local deadline=$((SECONDS + 15))
  while (( SECONDS < deadline )); do
    if grep -q "ServiceController listening on:" "${tmpdir}/controller.log" 2>/dev/null; then
      return 0
    fi
    if [[ -n "${controller_pid}" ]] && ! kill -0 "${controller_pid}" 2>/dev/null; then
      return 1
    fi
    sleep 0.1
  done
  return 1
}

wait_for_provider() {
  local id="$1"
  local log="$2"
  local deadline=$((SECONDS + 15))
  while (( SECONDS < deadline )); do
    if grep -q "Provider ${id} registered service /HELLO" "${log}" 2>/dev/null; then
      return 0
    fi
    if ! kill -0 "$(cat "${log}.pid")" 2>/dev/null; then
      return 1
    fi
    sleep 0.1
  done
  return 1
}

start_provider() {
  local id="$1"
  local message="$2"
  local log="${tmpdir}/provider-${id}.log"
  local pid_var="provider_${id,,}_pid"

  ./build/examples/App_Provider \
    --provider-id "${id}" \
    --ack-status reject \
    --ack-message "${message}" \
    --ack-payload "queue=100;gpu=busy;rank=99" \
    --response-payload "SHOULD_NOT_RUN_${id}" \
    >"${log}" 2>&1 &
  printf -v "${pid_var}" '%s' "$!"
  echo "${!pid_var}" >"${log}.pid"

  if ! wait_for_provider "${id}" "${log}"; then
    echo "Provider ${id} did not become ready"
    tail -n 120 "${log}" 2>/dev/null || true
    exit 1
  fi
}

./build/examples/App_ServiceController >"${tmpdir}/controller.log" 2>&1 &
controller_pid=$!
if ! wait_for_controller; then
  echo "Controller did not become ready"
  tail -n 120 "${tmpdir}/controller.log" 2>/dev/null || true
  exit 1
fi

start_provider A "QUEUE_FULL"
start_provider B "MODEL_UNAVAILABLE"

start_ms="$(date +%s%3N)"
timeout 12s ./build/examples/App_User \
  --custom-selection \
  --known-provider-ids A,B \
  --ack-timeout-ms 9000 \
  >"${tmpdir}/user.log" 2>&1
user_status=$?
end_ms="$(date +%s%3N)"
elapsed_ms=$((end_ms - start_ms))

echo "tmpdir=${tmpdir}"
echo "user_status=${user_status}"
echo "elapsed_ms=${elapsed_ms}"
echo "controller_log=${tmpdir}/controller.log"
echo "provider_a_log=${tmpdir}/provider-A.log"
echo "provider_b_log=${tmpdir}/provider-B.log"
echo "user_log=${tmpdir}/user.log"
echo
echo "--- provider A ---"
tail -n 140 "${tmpdir}/provider-A.log"
echo
echo "--- provider B ---"
tail -n 140 "${tmpdir}/provider-B.log"
echo
echo "--- user ---"
tail -n 220 "${tmpdir}/user.log"
if [[ -f "${tmpdir}/nfd.log" ]]; then
  echo
  echo "--- nfd ---"
  tail -n 80 "${tmpdir}/nfd.log"
fi

if [[ "${user_status}" -eq 0 ]] &&
   [[ "${elapsed_ms}" -lt 9000 ]] &&
   grep -q "Provider A publishing HELLO ACK status=false message=QUEUE_FULL" "${tmpdir}/provider-A.log" &&
   grep -q "Provider B publishing HELLO ACK status=false message=MODEL_UNAVAILABLE" "${tmpdir}/provider-B.log" &&
   grep -q "event=NEGATIVE_ACK_RECORDED .*providerName=/example/hello/provider/A .*reason=QUEUE_FULL" "${tmpdir}/user.log" &&
   grep -q "event=NEGATIVE_ACK_RECORDED .*providerName=/example/hello/provider/B .*reason=MODEL_UNAVAILABLE" "${tmpdir}/user.log" &&
   grep -q "event=NEGATIVE_ACK_EARLY_STOP_ALL_KNOWN_PROVIDERS" "${tmpdir}/user.log" &&
   grep -q "HELLO request timed out" "${tmpdir}/user.log" &&
   ! grep -q "publishing final response" "${tmpdir}/provider-A.log" &&
   ! grep -q "publishing final response" "${tmpdir}/provider-B.log"; then
  echo
  echo "NEGATIVE_ACK_EARLY_STOP_REGRESSION=PASS"
  exit 0
fi

echo
echo "NEGATIVE_ACK_EARLY_STOP_REGRESSION=FAIL"
exit 1
