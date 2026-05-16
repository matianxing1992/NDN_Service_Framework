#!/usr/bin/env bash
set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmpdir="$(mktemp -d /tmp/ndnsf-hello-ack-payload.XXXXXX)"

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

./build/examples/App_ServiceController >"${tmpdir}/controller.log" 2>&1 &
controller_pid=$!
sleep 2

./build/examples/App_Provider >"${tmpdir}/provider.log" 2>&1 &
provider_pid=$!
sleep 4

timeout 35s ./build/examples/App_User >"${tmpdir}/user.log" 2>&1
user_status=$?
sleep 2

echo "tmpdir=${tmpdir}"
echo "user_status=${user_status}"
echo
echo "--- controller ---"
tail -n 100 "${tmpdir}/controller.log"
echo
echo "--- provider ---"
tail -n 160 "${tmpdir}/provider.log"
echo
echo "--- user ---"
tail -n 160 "${tmpdir}/user.log"

if grep -q "Received HELLO request" "${tmpdir}/provider.log" &&
   grep -q "Publishing HELLO ACK payload: queue=0;gpu=idle;model=hello-v1" "${tmpdir}/provider.log" &&
   grep -q "payload=queue=0;gpu=idle;model=hello-v1" "${tmpdir}/user.log" &&
   grep -q "Received response: HELLO" "${tmpdir}/user.log"; then
  echo
  echo "HELLO_ACK_PAYLOAD_REGRESSION=PASS"
  exit 0
fi

echo
echo "HELLO_ACK_PAYLOAD_REGRESSION=FAIL"
exit 1
