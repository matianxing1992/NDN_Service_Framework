#!/usr/bin/env bash
set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmpdir="$(mktemp -d /tmp/ndnsf-token-cert-bootstrap.XXXXXX)"

controller_pid=""
provider_pid=""
nfd_started="false"

cleanup() {
  if [[ -n "${provider_pid}" ]]; then
    kill "${provider_pid}" 2>/dev/null || true
    wait "${provider_pid}" 2>/dev/null || true
  fi
  if [[ -n "${controller_pid}" ]]; then
    kill "${controller_pid}" 2>/dev/null || true
    wait "${controller_pid}" 2>/dev/null || true
  fi
  if [[ "${nfd_started}" == "true" ]]; then
    nfd-stop >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

cd "${repo_root}"
export LD_LIBRARY_PATH="${repo_root}/build:${LD_LIBRARY_PATH:-}"
export HOME="${tmpdir}/home"
export NDNSF_CONFIG="${tmpdir}/ndnsf.conf"
export NDNSF_SESSION_BASE="$(( $(date +%s) + $$ ))"
export NDN_LOG="${NDN_LOG:-ndn_service_framework.*=INFO}"
mkdir -p "${HOME}"

if ! pgrep -x nfd >/dev/null 2>&1; then
  nfd-start >"${tmpdir}/nfd.log" 2>&1
  nfd_started="true"
  sleep 2
fi

./build/examples/App_ServiceController \
  --bootstrap-token-file examples/hello.bootstrap-tokens \
  >"${tmpdir}/controller.log" 2>&1 &
controller_pid=$!

deadline=$((SECONDS + 15))
while (( SECONDS < deadline )); do
  if grep -q "ServiceController listening on:" "${tmpdir}/controller.log" 2>/dev/null; then
    break
  fi
  sleep 0.1
done

timeout 12s ./build/examples/App_User \
  --bootstrap-token wrong-token \
  --bootstrap-name /example/hello/user \
  >"${tmpdir}/wrong-user.log" 2>&1
wrong_status=$?

timeout 12s ./build/examples/App_User \
  --bootstrap-token user-token-045 \
  --bootstrap-name /example/hello/provider \
  >"${tmpdir}/wrong-name-user.log" 2>&1
wrong_name_status=$?

./build/examples/App_Provider \
  --bootstrap-token provider-token-045 \
  --bootstrap-name /example/hello/provider \
  >"${tmpdir}/provider.log" 2>&1 &
provider_pid=$!

deadline=$((SECONDS + 20))
while (( SECONDS < deadline )); do
  if grep -q "Provider .* registered service /HELLO" "${tmpdir}/provider.log" 2>/dev/null; then
    break
  fi
  sleep 0.1
done

timeout 30s ./build/examples/App_User \
  --bootstrap-token user-token-045 \
  --bootstrap-name /example/hello/user \
  >"${tmpdir}/user.log" 2>&1
user_status=$?

timeout 30s ./build/examples/App_User \
  --bootstrap-token user-token-045 \
  --bootstrap-name /example/hello/user \
  >"${tmpdir}/user-reuse.log" 2>&1
reuse_status=$?
sleep 1

user_issued_count=$(grep -c "NDNSF_CERT_BOOTSTRAP_ISSUED identity=/example/hello/user" \
  "${tmpdir}/controller.log" || true)

echo "tmpdir=${tmpdir}"
echo "wrong_status=${wrong_status}"
echo "wrong_name_status=${wrong_name_status}"
echo "user_status=${user_status}"
echo "reuse_status=${reuse_status}"
echo "user_issued_count=${user_issued_count}"
echo
echo "--- controller ---"
tail -n 160 "${tmpdir}/controller.log"
echo
echo "--- wrong user ---"
tail -n 100 "${tmpdir}/wrong-user.log"
echo
echo "--- wrong name user ---"
tail -n 100 "${tmpdir}/wrong-name-user.log"
echo
echo "--- provider ---"
tail -n 140 "${tmpdir}/provider.log"
echo
echo "--- user ---"
tail -n 160 "${tmpdir}/user.log"
echo
echo "--- user reuse ---"
tail -n 160 "${tmpdir}/user-reuse.log"
if [[ -f "${tmpdir}/nfd.log" ]]; then
  echo
  echo "--- nfd ---"
  tail -n 80 "${tmpdir}/nfd.log"
fi

if [[ "${wrong_status}" -ne 0 ]] &&
   [[ "${wrong_name_status}" -ne 0 ]] &&
   [[ "${user_status}" -eq 0 ]] &&
   [[ "${reuse_status}" -eq 0 ]] &&
   [[ "${user_issued_count}" -eq 1 ]] &&
   grep -q "NDNSF_CERT_BOOTSTRAP_REFUSED identity=/example/hello/user reason=token-mismatch" "${tmpdir}/controller.log" &&
   grep -q "NDNSF_CERT_BOOTSTRAP_REFUSED identity=/example/hello/provider reason=token-mismatch" "${tmpdir}/controller.log" &&
   grep -q "NDNSF_CERT_BOOTSTRAP_ISSUED identity=/example/hello/provider" "${tmpdir}/controller.log" &&
   grep -q "NDNSF_CERT_BOOTSTRAP_ISSUED identity=/example/hello/user" "${tmpdir}/controller.log" &&
   grep -q "NDNSF_CERT_BOOTSTRAP_INSTALLED identity=/example/hello/provider" "${tmpdir}/provider.log" &&
   grep -q "NDNSF_CERT_BOOTSTRAP_INSTALLED identity=/example/hello/user" "${tmpdir}/user.log" &&
   grep -q "NDNSF_CERT_BOOTSTRAP_REUSED identity=/example/hello/user" "${tmpdir}/user-reuse.log" &&
   grep -q "Installed provider permission provider=/example/hello/provider service=/HELLO" "${tmpdir}/provider.log" &&
   grep -q "Installed user permission provider=/example/hello/provider service=/HELLO" "${tmpdir}/user.log" &&
   grep -q "Installed user permission provider=/example/hello/provider service=/HELLO" "${tmpdir}/user-reuse.log" &&
   grep -q "Received HELLO request" "${tmpdir}/provider.log" &&
   grep -q "Received response: HELLO" "${tmpdir}/user.log" &&
   grep -q "Received response: HELLO" "${tmpdir}/user-reuse.log"; then
  echo
  echo "TOKEN_CERTIFICATE_BOOTSTRAP_REGRESSION=PASS"
  exit 0
fi

echo
echo "TOKEN_CERTIFICATE_BOOTSTRAP_REGRESSION=FAIL"
exit 1
