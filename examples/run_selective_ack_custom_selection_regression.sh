#!/usr/bin/env bash
set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmpdir="$(mktemp -d /tmp/ndnsf-selective-ack-custom.XXXXXX)"

controller_pid=""
provider_a_pid=""
provider_b_pid=""
provider_c_pid=""
nfd_started="false"

cleanup() {
  for pid in "${provider_a_pid}" "${provider_b_pid}" "${provider_c_pid}" "${controller_pid}"; do
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
export NDN_LOG="${NDN_LOG:-ndn_service_framework.*=INFO}"

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

wait_for_runtime_logs() {
  local deadline=$((SECONDS + 10))
  while (( SECONDS < deadline )); do
    if grep -q "SELECTIVE_ACK_CUSTOM_SELECTION_REGRESSION=PASS" "${tmpdir}/user.log" 2>/dev/null &&
       grep -q "Provider B publishing final response: HELLO_FROM_B" "${tmpdir}/provider-B.log" 2>/dev/null; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

start_provider() {
  local id="$1"
  local status="$2"
  local message="$3"
  local payload="$4"
  local response="$5"
  local log="${tmpdir}/provider-${id}.log"
  local pid_var="provider_${id,,}_pid"

  ./build/examples/App_Provider \
    --provider-id "${id}" \
    --ack-status "${status}" \
    --ack-message "${message}" \
    --ack-payload "${payload}" \
    --response-payload "${response}" \
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
  exit 1
fi

start_provider A true "Provider A ready" "queue=5;gpu=busy;rank=3" "HELLO_FROM_A"
start_provider B true "Provider B ready" "queue=1;gpu=idle;rank=1" "HELLO_FROM_B"
start_provider C reject "PROVIDER_BUSY" "queue=99;gpu=busy;rank=99" "HELLO_FROM_C"

echo "All providers subscribed before user request"
timeout 35s ./build/examples/App_User \
  --custom-selection \
  --ack-timeout-ms 500 \
  --expect-response HELLO_FROM_B \
  >"${tmpdir}/user.log" 2>&1
user_status=$?
wait_for_runtime_logs || true

echo "tmpdir=${tmpdir}"
echo "user_status=${user_status}"
echo "controller_log=${tmpdir}/controller.log"
echo "provider_a_log=${tmpdir}/provider-A.log"
echo "provider_b_log=${tmpdir}/provider-B.log"
echo "provider_c_log=${tmpdir}/provider-C.log"
echo "user_log=${tmpdir}/user.log"
echo
echo "--- controller ---"
tail -n 120 "${tmpdir}/controller.log"
echo
echo "--- provider A ---"
tail -n 160 "${tmpdir}/provider-A.log"
echo
echo "--- provider B ---"
tail -n 160 "${tmpdir}/provider-B.log"
echo
echo "--- provider C ---"
tail -n 160 "${tmpdir}/provider-C.log"
echo
echo "--- user ---"
tail -n 220 "${tmpdir}/user.log"
if [[ -f "${tmpdir}/nfd.log" ]]; then
  echo
  echo "--- nfd ---"
  tail -n 80 "${tmpdir}/nfd.log"
fi

if [[ "${user_status}" -eq 0 ]] &&
   grep -q "Fetch user permissions: /example/hello/controller/NDNSF/PERMISSIONS/USER/example/hello/user" "${tmpdir}/user.log" &&
   grep -q "Installed user permission provider=/example/hello/provider/A service=/HELLO" "${tmpdir}/user.log" &&
   grep -q "Installed user permission provider=/example/hello/provider/B service=/HELLO" "${tmpdir}/user.log" &&
   grep -q "Installed user permission provider=/example/hello/provider/C service=/HELLO" "${tmpdir}/user.log" &&
   grep -q "Fetch provider permissions: /example/hello/controller/NDNSF/PERMISSIONS/PROVIDER/example/hello/provider/A" "${tmpdir}/provider-A.log" &&
   grep -q "Fetch provider permissions: /example/hello/controller/NDNSF/PERMISSIONS/PROVIDER/example/hello/provider/B" "${tmpdir}/provider-B.log" &&
   grep -q "Fetch provider permissions: /example/hello/controller/NDNSF/PERMISSIONS/PROVIDER/example/hello/provider/C" "${tmpdir}/provider-C.log" &&
   grep -q "Installed provider permission provider=/example/hello/provider/A service=/HELLO" "${tmpdir}/provider-A.log" &&
   grep -q "Installed provider permission provider=/example/hello/provider/B service=/HELLO" "${tmpdir}/provider-B.log" &&
   grep -q "Installed provider permission provider=/example/hello/provider/C service=/HELLO" "${tmpdir}/provider-C.log" &&
   grep -q "\\[PERMISSIONS/USER\\] Encrypted reply target=/example/hello/user" "${tmpdir}/controller.log" &&
   grep -q "\\[PERMISSIONS/PROVIDER\\] Encrypted reply target=/example/hello/provider/A" "${tmpdir}/controller.log" &&
   grep -q "\\[PERMISSIONS/PROVIDER\\] Encrypted reply target=/example/hello/provider/B" "${tmpdir}/controller.log" &&
   grep -q "\\[PERMISSIONS/PROVIDER\\] Encrypted reply target=/example/hello/provider/C" "${tmpdir}/controller.log" &&
   ! grep -R -q "isAuthorized[[:space:]]*=[[:space:]]*true" "${tmpdir}" &&
   ! rg -n "isAuthorized[[:space:]]*=[[:space:]]*true" ndn-service-framework examples >/dev/null &&
   grep -q "Provider A selective ACK handler received request" "${tmpdir}/provider-A.log" &&
   grep -q "Provider A request received timestampMs=" "${tmpdir}/provider-A.log" &&
   grep -q "Provider A publishing HELLO ACK status=true .*payload=backlog=0;processed10s=0;queue=0;rank=3" "${tmpdir}/provider-A.log" &&
   grep -q "Provider B selective ACK handler received request" "${tmpdir}/provider-B.log" &&
   grep -q "Provider B request received timestampMs=" "${tmpdir}/provider-B.log" &&
   grep -q "Provider B publishing HELLO ACK status=true .*payload=backlog=0;processed10s=0;queue=0;rank=1" "${tmpdir}/provider-B.log" &&
   grep -q "Provider C selective ACK handler received request" "${tmpdir}/provider-C.log" &&
   grep -q "Provider C request received timestampMs=" "${tmpdir}/provider-C.log" &&
   grep -q "Provider C selective ACK handler rejected request" "${tmpdir}/provider-C.log" &&
   grep -q "customSelectionStrategy ran after ackTimeoutMs=500" "${tmpdir}/user.log" &&
   grep -q "customSelectionStrategy candidate providerName=/example/hello/provider/A status=true .*payload=backlog=0;processed10s=0;queue=0;rank=3" "${tmpdir}/user.log" &&
   grep -q "customSelectionStrategy candidate providerName=/example/hello/provider/B status=true .*payload=backlog=0;processed10s=0;queue=0;rank=1" "${tmpdir}/user.log" &&
   grep -q "customSelectionStrategy candidate providerName=/example/hello/provider/C status=false .*payload=backlog=0;processed10s=0;queue=0;rank=99" "${tmpdir}/user.log" &&
   grep -q "collected ACK payload provider=A queue=0 rank=3" "${tmpdir}/user.log" &&
   grep -q "collected ACK payload provider=B queue=0 rank=1" "${tmpdir}/user.log" &&
   grep -q "customSelectionStrategy rejected provider=C status=0" "${tmpdir}/user.log" &&
   grep -q "customSelectionStrategy selected providerName=/example/hello/provider/B" "${tmpdir}/user.log" &&
   grep -q "Provider B publishing final response: HELLO_FROM_B" "${tmpdir}/provider-B.log" &&
   ! grep -q "Provider A publishing final response" "${tmpdir}/provider-A.log" &&
   ! grep -q "Provider C publishing final response" "${tmpdir}/provider-C.log" &&
   grep -q "Received response: HELLO_FROM_B" "${tmpdir}/user.log" &&
   grep -q "SELECTIVE_ACK_CUSTOM_SELECTION_REGRESSION=PASS" "${tmpdir}/user.log"; then
  echo
  echo "SELECTIVE_ACK_CUSTOM_SELECTION_REGRESSION=PASS"
  exit 0
fi

echo
echo "SELECTIVE_ACK_CUSTOM_SELECTION_REGRESSION=FAIL"
exit 1
