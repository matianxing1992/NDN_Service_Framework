#!/usr/bin/env bash
set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmpdir="$(mktemp -d /tmp/ndnsf-python-token-cert-bootstrap.XXXXXX)"
nfd_started="false"
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
  if [[ "${nfd_started}" == "true" ]]; then
    nfd-stop >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

cd "${repo_root}"
export LD_LIBRARY_PATH="${repo_root}/build:${LD_LIBRARY_PATH:-}"
export PYTHONPATH="${repo_root}/pythonWrapper:${repo_root}:${PYTHONPATH:-}"
export PYTHONDONTWRITEBYTECODE=1
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

python3 examples/python_token_certificate_bootstrap_smoke.py --role controller \
  >"${tmpdir}/controller.log" 2>&1 &
controller_pid=$!

deadline=$((SECONDS + 15))
while (( SECONDS < deadline )); do
  if grep -q "ServiceController listening on:" "${tmpdir}/controller.log" 2>/dev/null; then
    break
  fi
  sleep 0.1
done

python3 examples/python_token_certificate_bootstrap_smoke.py --role provider \
  >"${tmpdir}/provider.log" 2>&1 &
provider_pid=$!

deadline=$((SECONDS + 20))
while (( SECONDS < deadline )); do
  if grep -q "Provider .* registered service /HELLO" "${tmpdir}/provider.log" 2>/dev/null; then
    break
  fi
  sleep 0.1
done

timeout 30s python3 examples/python_token_certificate_bootstrap_smoke.py \
  --role user --label PYTHON_TOKEN_BOOTSTRAP_FIRST_REQUEST \
  >"${tmpdir}/user.log" 2>&1
first_status=$?

timeout 30s python3 examples/python_token_certificate_bootstrap_smoke.py \
  --role user --label PYTHON_TOKEN_BOOTSTRAP_REUSE_REQUEST \
  >"${tmpdir}/user-reuse.log" 2>&1
reuse_status=$?
sleep 1

user_issued_count=$(grep -c "NDNSF_CERT_BOOTSTRAP_ISSUED identity=/example/hello/user" \
  "${tmpdir}/controller.log" || true)

echo "tmpdir=${tmpdir}"
echo "first_status=${first_status}"
echo "reuse_status=${reuse_status}"
echo "user_issued_count=${user_issued_count}"
echo
echo "--- controller ---"
tail -n 180 "${tmpdir}/controller.log"
echo
echo "--- provider ---"
tail -n 160 "${tmpdir}/provider.log"
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

if [[ "${first_status}" -eq 0 ]] &&
   [[ "${reuse_status}" -eq 0 ]] &&
   [[ "${user_issued_count}" -eq 1 ]] &&
   grep -q "NDNSF_CERT_BOOTSTRAP_ISSUED identity=/example/hello/provider" "${tmpdir}/controller.log" &&
   grep -q "NDNSF_CERT_BOOTSTRAP_ISSUED identity=/example/hello/user" "${tmpdir}/controller.log" &&
   grep -q "NDNSF_CERT_BOOTSTRAP_INSTALLED identity=/example/hello/provider" "${tmpdir}/provider.log" &&
   grep -q "NDNSF_CERT_BOOTSTRAP_INSTALLED identity=/example/hello/user" "${tmpdir}/user.log" &&
   grep -q "NDNSF_CERT_BOOTSTRAP_REUSED identity=/example/hello/user" "${tmpdir}/user-reuse.log" &&
   grep -q "PYTHON_TOKEN_BOOTSTRAP_FIRST_REQUEST=OK" "${tmpdir}/user.log" &&
   grep -q "PYTHON_TOKEN_BOOTSTRAP_REUSE_REQUEST=OK" "${tmpdir}/user-reuse.log"; then
  echo
  echo "PYTHON_TOKEN_CERTIFICATE_BOOTSTRAP_REGRESSION=PASS"
  exit 0
fi

echo
echo "PYTHON_TOKEN_CERTIFICATE_BOOTSTRAP_REGRESSION=FAIL"
exit 1
