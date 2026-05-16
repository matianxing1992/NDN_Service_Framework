#!/usr/bin/env bash
set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmpdir="$(mktemp -d /tmp/ndnsf-nac-abe-routing.XXXXXX)"

controller_pid=""
provider_pid=""

cleanup() {
  if [[ -n "${provider_pid}" ]]; then
    kill "${provider_pid}" 2>/dev/null || true
    wait "${provider_pid}" 2>/dev/null || true
  fi
  if [[ -n "${controller_pid}" ]]; then
    kill "${controller_pid}" 2>/dev/null || true
    wait "${controller_pid}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

cd "${repo_root}"
export LD_LIBRARY_PATH="${repo_root}/build:${LD_LIBRARY_PATH:-}"
export NDNSF_CONFIG="${tmpdir}/ndnsf.conf"
export NDNSF_SESSION_BASE="$(( $(date +%s) + $$ ))"
export NDN_LOG="${NDN_LOG:-ndn_service_framework.*=INFO}"

./build/examples/App_ServiceController >"${tmpdir}/controller.log" 2>&1 &
controller_pid=$!
sleep 2

./build/examples/App_Provider >"${tmpdir}/provider.log" 2>&1 &
provider_pid=$!
sleep 4

timeout 30s ./build/examples/App_User >"${tmpdir}/user.log" 2>&1
user_status=$?
sleep 2

echo "tmpdir=${tmpdir}"
echo "user_status=${user_status}"
echo
echo "--- controller ---"
tail -n 100 "${tmpdir}/controller.log"
echo
echo "--- provider ---"
tail -n 180 "${tmpdir}/provider.log"
echo
echo "--- user ---"
tail -n 180 "${tmpdir}/user.log"

if [[ "${user_status}" -eq 0 ]] &&
   grep -q "GetAttributesByName: messageName=.*/NDNSF/REQUEST/1/HELLO/.* attributes=/SERVICE/HELLO" "${tmpdir}/user.log" &&
   grep -q "GetAttributesByName: messageName=.*/NDNSF/COORDINATION/3/example/hello/provider/1/HELLO/.* attributes=/SERVICE/HELLO" "${tmpdir}/user.log" &&
   grep -q "GetAttributesByName: messageName=.*/NDNSF/ACK/3/example/hello/user/1/HELLO/.* attributes=/PERMISSION/HELLO" "${tmpdir}/provider.log" &&
   grep -q "GetAttributesByName: messageName=.*/NDNSF/RESPONSE/3/example/hello/user/1/HELLO/.* attributes=/PERMISSION/HELLO" "${tmpdir}/provider.log" &&
   grep -q "Received response: HELLO" "${tmpdir}/user.log"; then
  echo
  echo "NAC_ABE_ATTRIBUTE_ROUTING_REGRESSION=PASS"
  exit 0
fi

echo
echo "NAC_ABE_ATTRIBUTE_ROUTING_REGRESSION=FAIL"
exit 1
