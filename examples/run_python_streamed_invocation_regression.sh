#!/usr/bin/env bash
set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${NDNSF_BUILD_DIR:-${repo_root}/build}"
tmpdir="$(mktemp -d /tmp/ndnsf-python-stream.XXXXXX)"

controller_pid=""
provider_pid=""
nfd_started="false"

cleanup() {
  for pid in "${provider_pid}" "${controller_pid}"; do
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
export LD_LIBRARY_PATH="${build_dir}:${LD_LIBRARY_PATH:-}"
export PYTHONPATH="${repo_root}/pythonWrapper:${repo_root}/NDNSF-DistributedRepo/pythonWrapper:${repo_root}/NDNSF-DistributedInference:${repo_root}/Experiments${PYTHONPATH:+:${PYTHONPATH}}"
export NDNSF_DISABLE_NDNSD=1
export NDNSF_CONFIG="${tmpdir}/ndnsf.conf"
export NDNSF_SESSION_BASE="$(( $(date +%s) + $$ ))"
export NDN_LOG="${NDN_LOG:-ndn_service_framework.*=INFO}"

# A stale nfd process can survive after its UNIX socket has disappeared.  A
# process-name check alone then skips startup and makes every application
# report an unrelated socket EOF.  Require both the socket and a successful
# management query; otherwise stop only the stale NFD service and start a
# fresh one for this regression.
nfd_ready="false"
if [[ -S /run/nfd/nfd.sock ]] && nfdc status >/dev/null 2>&1; then
  nfd_ready="true"
fi
if [[ "${nfd_ready}" != "true" ]]; then
  if pgrep -x nfd >/dev/null 2>&1; then
    nfd-stop >/dev/null 2>&1 || true
  fi
  nfd-start >"${tmpdir}/nfd.log" 2>&1
  nfd_started="true"
  sleep 2
fi
nfdc strategy set /example/hello/group /localhost/nfd/strategy/multicast/v=5 \
  >/dev/null 2>&1 || true

"${build_dir}/examples/App_ServiceController" \
  >"${tmpdir}/controller.log" 2>&1 &
controller_pid=$!

deadline=$((SECONDS + 15))
while ! grep -q "ServiceController listening on:" "${tmpdir}/controller.log" 2>/dev/null; do
  if ! kill -0 "${controller_pid}" 2>/dev/null || (( SECONDS >= deadline )); then
    echo "Python stream Controller did not become ready"
    tail -n 160 "${tmpdir}/controller.log" 2>/dev/null || true
    exit 1
  fi
  sleep 0.1
done

python3 examples/python/streamed_invocation_regression.py provider \
  >"${tmpdir}/provider.log" 2>&1 &
provider_pid=$!

deadline=$((SECONDS + 20))
while ! grep -q "PYTHON_STREAM_PROVIDER_START" "${tmpdir}/provider.log" 2>/dev/null; do
  if ! kill -0 "${provider_pid}" 2>/dev/null || (( SECONDS >= deadline )); then
    echo "Python stream Provider did not start"
    tail -n 200 "${tmpdir}/provider.log" 2>/dev/null || true
    exit 1
  fi
  sleep 0.1
done
sleep 4

timeout 35s python3 examples/python/streamed_invocation_regression.py user \
  >"${tmpdir}/user.log" 2>&1
user_status=$?

# The lifetime probe deliberately runs after the Provider handler returns.
# Wait only for that post-return marker; never hide a missing fence behind an
# unbounded sleep or a successful User exit.
probe_deadline=$((SECONDS + 2))
while [[ "${user_status}" -eq 0 ]] &&
      ! grep -q "PYTHON_STREAM_WRITER_FENCED=" "${tmpdir}/provider.log" 2>/dev/null &&
      (( SECONDS < probe_deadline )); do
  sleep 0.05
done

echo "tmpdir=${tmpdir}"
echo "user_status=${user_status}"
echo "--- provider ---"
tail -n 200 "${tmpdir}/provider.log" 2>/dev/null || true
echo "--- user ---"
tail -n 200 "${tmpdir}/user.log" 2>/dev/null || true

if [[ "${user_status}" -eq 0 ]] &&
   grep -q "PYTHON_STREAM_PROVIDER_FINISHED" "${tmpdir}/provider.log" &&
   grep -q "PYTHON_STREAM_WRITER_FENCED=PASS" "${tmpdir}/provider.log" &&
   grep -q "PYTHON_STREAM_USER_PASS" "${tmpdir}/user.log"; then
  echo "PYTHON_STREAMED_INVOCATION_REGRESSION=PASS"
  exit 0
fi

echo "--- controller ---"
tail -n 160 "${tmpdir}/controller.log" 2>/dev/null || true
echo "PYTHON_STREAMED_INVOCATION_REGRESSION=FAIL"
exit 1
