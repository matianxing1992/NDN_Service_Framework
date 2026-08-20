#!/usr/bin/env bash
set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmpdir="$(mktemp -d /tmp/ndnsf-large-data.XXXXXX)"
plaintext="$(python3 - <<'PY'
print("large-image-" + ("0123456789abcdef" * 512), end="")
PY
)"
name_file="${tmpdir}/encrypted-data-name.txt"

controller_pid=""
bootstrap_provider_pid=""
user_pid=""

cleanup() {
  if [[ -n "${user_pid}" ]]; then
    kill "${user_pid}" 2>/dev/null || true
    wait "${user_pid}" 2>/dev/null || true
  fi
  if [[ -n "${bootstrap_provider_pid}" ]]; then
    kill "${bootstrap_provider_pid}" 2>/dev/null || true
    wait "${bootstrap_provider_pid}" 2>/dev/null || true
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

./build/examples/App_ServiceController \
  --policy-file examples/large-data-validation.policies \
  >"${tmpdir}/controller.log" 2>&1 &
controller_pid=$!
sleep 2

# ServiceUser obtains the NAC-ABE decryption key during construction.  Keep a
# real Provider alive long enough to request that key before starting the
# publisher; otherwise the publisher blocks before the large-data test starts.
./build/examples/App_Provider \
  >"${tmpdir}/bootstrap-provider.log" 2>&1 &
bootstrap_provider_pid=$!
sleep 4

./build/examples/App_User \
  --large-data-publish-test \
  --large-data-plaintext "${plaintext}" \
  --large-data-name-file "${name_file}" \
  >"${tmpdir}/user.log" 2>&1 &
user_pid=$!

for _ in $(seq 1 30); do
  if [[ -s "${name_file}" ]]; then
    break
  fi
  if ! kill -0 "${user_pid}" 2>/dev/null; then
    break
  fi
  sleep 1
done

encrypted_name=""
if [[ -s "${name_file}" ]]; then
  encrypted_name="$(head -n 1 "${name_file}")"
fi

# The bootstrap Provider is only a DKEY/bootstrap dependency.  Stop it before
# the fetch probes so the authorized and unauthorized fetches are isolated.
if [[ -n "${bootstrap_provider_pid}" ]]; then
  kill "${bootstrap_provider_pid}" 2>/dev/null || true
  wait "${bootstrap_provider_pid}" 2>/dev/null || true
  bootstrap_provider_pid=""
fi

authorized_status=1
unauthorized_status=1
if [[ -n "${encrypted_name}" ]]; then
  timeout 30s ./build/examples/App_Provider \
    --large-data-fetch-test \
    --large-data-name "${encrypted_name}" \
    --expect-large-data-plaintext "${plaintext}" \
    >"${tmpdir}/provider-authorized.log" 2>&1
  authorized_status=$?

  timeout 30s ./build/examples/App_Provider \
    --provider-id unauthorized \
    --large-data-fetch-test \
    --large-data-name "${encrypted_name}" \
    --expect-large-data-failure \
    >"${tmpdir}/provider-unauthorized.log" 2>&1
  unauthorized_status=$?
fi

echo "tmpdir=${tmpdir}"
echo "encrypted_name=${encrypted_name}"
echo "plaintext_bytes=${#plaintext}"
echo "authorized_status=${authorized_status}"
echo "unauthorized_status=${unauthorized_status}"
echo
echo "--- controller ---"
tail -n 120 "${tmpdir}/controller.log"
echo
echo "--- user ---"
tail -n 120 "${tmpdir}/user.log"
echo
echo "--- bootstrap provider ---"
tail -n 120 "${tmpdir}/bootstrap-provider.log" 2>/dev/null || true
echo
echo "--- provider authorized ---"
tail -n 160 "${tmpdir}/provider-authorized.log" 2>/dev/null || true
echo
echo "--- provider unauthorized ---"
tail -n 160 "${tmpdir}/provider-unauthorized.log" 2>/dev/null || true

if [[ -n "${encrypted_name}" ]] &&
   [[ "${authorized_status}" -eq 0 ]] &&
   [[ "${unauthorized_status}" -eq 0 ]] &&
   [[ "${encrypted_name}" != *"/seg="* ]] &&
   grep -q "LARGE_DATA_PUBLISH_SUCCESS name=" "${tmpdir}/user.log" &&
   grep -Eq "LARGE_DATA_PUBLISH_SEGMENTS .*segments=([2-9]|[1-9][0-9]+)" "${tmpdir}/user.log" &&
   grep -q "LARGE_DATA_FETCH_SUCCESS plaintext=${plaintext}" "${tmpdir}/provider-authorized.log" &&
   grep -q "LARGE_DATA_UNAUTHORIZED_FAILURE_CLEAN error=" "${tmpdir}/provider-unauthorized.log"; then
  echo
  echo "LARGE_DATA_HELPER_VALIDATION=PASS" | tee -a "${tmpdir}/user.log"
  exit 0
fi

echo
echo "LARGE_DATA_HELPER_VALIDATION=FAIL"
exit 1
