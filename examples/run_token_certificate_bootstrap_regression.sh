#!/usr/bin/env bash
set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmpdir="$(mktemp -d /tmp/ndnsf-token-cert-bootstrap.XXXXXX)"

source "${repo_root}/examples/common_regression.sh"

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
  ndnsf_stop_nfd_if_started
}
trap cleanup EXIT

cd "${repo_root}"
export LD_LIBRARY_PATH="${repo_root}/build:${LD_LIBRARY_PATH:-}"
export HOME="${tmpdir}/home"
export NDNSF_CONFIG="${tmpdir}/ndnsf.conf"
export NDNSF_SESSION_BASE="$(( $(date +%s) + $$ ))"
export NDN_LOG="${NDN_LOG:-ndn_service_framework.*=INFO}"
mkdir -p "${HOME}"

ndnsf_start_nfd_if_needed "${tmpdir}/nfd.log" || {
  echo "NFD socket /run/nfd/nfd.sock is unavailable" >&2
  ndnsf_dump_tail "nfd" "${tmpdir}/nfd.log" 80
  exit 1
}

generated_token_file="${tmpdir}/generated.bootstrap-tokens"
timeout 4s ./build/examples/App_ServiceController \
  --bootstrap-token-file "${generated_token_file}" \
  >"${tmpdir}/generated-controller.log" 2>&1
generated_controller_status=$?
generated_entry_count=$(awk 'NF >= 2 && $1 !~ /^#/ { count++ } END { print count + 0 }' \
  "${generated_token_file}" 2>/dev/null || echo 0)
generated_bad_token_count=$(awk 'NF >= 2 && $1 !~ /^#/ && length($2) != 8 { count++ } END { print count + 0 }' \
  "${generated_token_file}" 2>/dev/null || echo 0)

./build/examples/App_ServiceController \
  --bootstrap-token-file examples/hello.bootstrap-tokens \
  >"${tmpdir}/controller.log" 2>&1 &
controller_pid=$!

ndnsf_wait_for_log "${tmpdir}/controller.log" "ServiceController listening on:" 15 || true

timeout 12s ./build/examples/App_User \
  --bootstrap-token wrong-token \
  >"${tmpdir}/wrong-user.log" 2>&1
wrong_status=$?

timeout 12s ./build/examples/App_User \
  --bootstrap-token user045A \
  --bootstrap-name /example/hello/provider \
  >"${tmpdir}/wrong-name-user.log" 2>&1
wrong_name_status=$?

timeout 12s ./build/examples/App_CertificateBootstrapTamper \
  --bootstrap-token user045A \
  >"${tmpdir}/tampered-proof-user.log" 2>&1
tampered_status=$?

timeout 12s ./build/examples/App_CertificateBootstrapTamper \
  --bootstrap-token user045A \
  --valid-request \
  >"${tmpdir}/valid-token-probe-1.log" 2>&1
valid_probe_1_status=$?

timeout 12s ./build/examples/App_CertificateBootstrapTamper \
  --bootstrap-token user045A \
  --valid-request \
  >"${tmpdir}/valid-token-probe-2.log" 2>&1
valid_probe_2_status=$?

./build/examples/App_Provider \
  --bootstrap-token prov045A \
  >"${tmpdir}/provider.log" 2>&1 &
provider_pid=$!

ndnsf_wait_for_log "${tmpdir}/provider.log" "Provider .* registered service /HELLO" 20 || true

timeout 30s ./build/examples/App_User \
  --bootstrap-token user045A \
  >"${tmpdir}/user.log" 2>&1
user_status=$?

timeout 30s ./build/examples/App_User \
  --bootstrap-token user045A \
  >"${tmpdir}/user-reuse.log" 2>&1
reuse_status=$?
sleep 1

user_issued_count=$(grep -c "NDNSF_CERT_BOOTSTRAP_ISSUED identity=/example/hello/user" \
  "${tmpdir}/controller.log" || true)

echo "tmpdir=${tmpdir}"
echo "wrong_status=${wrong_status}"
echo "wrong_name_status=${wrong_name_status}"
echo "tampered_status=${tampered_status}"
echo "valid_probe_1_status=${valid_probe_1_status}"
echo "valid_probe_2_status=${valid_probe_2_status}"
echo "user_status=${user_status}"
echo "reuse_status=${reuse_status}"
echo "user_issued_count=${user_issued_count}"
echo "generated_controller_status=${generated_controller_status}"
echo "generated_entry_count=${generated_entry_count}"
echo "generated_bad_token_count=${generated_bad_token_count}"
echo "generated_token_file=${generated_token_file}"
echo
echo "--- generated controller ---"
tail -n 80 "${tmpdir}/generated-controller.log"
if [[ -f "${generated_token_file}" ]]; then
  echo "--- generated token file ---"
  cat "${generated_token_file}"
fi
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
echo "--- tampered proof user ---"
tail -n 100 "${tmpdir}/tampered-proof-user.log"
echo
echo "--- valid token probe 1 ---"
tail -n 100 "${tmpdir}/valid-token-probe-1.log"
echo
echo "--- valid token probe 2 ---"
tail -n 100 "${tmpdir}/valid-token-probe-2.log"
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
   [[ "${tampered_status}" -eq 0 ]] &&
   [[ "${valid_probe_1_status}" -eq 0 ]] &&
   [[ "${valid_probe_2_status}" -eq 0 ]] &&
   [[ "${user_status}" -eq 0 ]] &&
   [[ "${reuse_status}" -eq 0 ]] &&
   [[ "${user_issued_count}" -eq 3 ]] &&
   [[ "${generated_controller_status}" -eq 124 ]] &&
   [[ "${generated_entry_count}" -eq 5 ]] &&
   [[ "${generated_bad_token_count}" -eq 0 ]] &&
   grep -q "NDNSF_CERT_BOOTSTRAP_TOKEN_FILE_GENERATED" "${tmpdir}/generated-controller.log" &&
   grep -q "NDNSF_CERT_BOOTSTRAP_REFUSED identity=/example/hello/user reason=token-mismatch" "${tmpdir}/controller.log" &&
   grep -q "NDNSF_CERT_BOOTSTRAP_REFUSED identity=/example/hello/provider reason=token-mismatch" "${tmpdir}/controller.log" &&
   grep -q "NDNSF_CERT_BOOTSTRAP_REFUSED identity=/example/hello/user reason=request-proof-invalid" "${tmpdir}/controller.log" &&
   grep -q "TAMPERED_BOOTSTRAP_PROOF_REJECTED=OK" "${tmpdir}/tampered-proof-user.log" &&
   grep -q "PRECONFIGURED_TOKEN_BOOTSTRAP_ACCEPTED=OK" "${tmpdir}/valid-token-probe-1.log" &&
   grep -q "PRECONFIGURED_TOKEN_BOOTSTRAP_ACCEPTED=OK" "${tmpdir}/valid-token-probe-2.log" &&
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
