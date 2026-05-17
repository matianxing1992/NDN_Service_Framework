#!/usr/bin/env bash
set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmpdir="$(mktemp -d /tmp/ndnsf-local-nfd-latency.XXXXXX)"
csv_path="${tmpdir}/service-latency.csv"

count="${NDNSF_BENCHMARK_COUNT:-100}"
warmup="${NDNSF_BENCHMARK_WARMUP:-5}"
timeout_ms="${NDNSF_BENCHMARK_TIMEOUT_MS:-20000}"
ack_timeout_ms="${NDNSF_BENCHMARK_ACK_TIMEOUT_MS:-100}"
interval_ms="${NDNSF_BENCHMARK_INTERVAL_MS:-1000}"
strategy="${NDNSF_BENCHMARK_STRATEGY:-no-coordination}"

controller_pid=""
provider_pid=""

kill_stale_apps() {
  pkill -f "${repo_root}/build/examples/App_ServiceController" 2>/dev/null || true
  pkill -f "${repo_root}/build/examples/App_Provider" 2>/dev/null || true
  pkill -f "${repo_root}/build/examples/App_User" 2>/dev/null || true
  pkill -f "./build/examples/App_ServiceController" 2>/dev/null || true
  pkill -f "./build/examples/App_Provider" 2>/dev/null || true
  pkill -f "./build/examples/App_User" 2>/dev/null || true
}

cleanup() {
  if [[ -n "${provider_pid}" ]]; then
    kill "${provider_pid}" 2>/dev/null || true
    wait "${provider_pid}" 2>/dev/null || true
  fi
  if [[ -n "${controller_pid}" ]]; then
    kill "${controller_pid}" 2>/dev/null || true
    wait "${controller_pid}" 2>/dev/null || true
  fi
  kill_stale_apps
}
trap cleanup EXIT

cd "${repo_root}"
export LD_LIBRARY_PATH="${repo_root}/build:${LD_LIBRARY_PATH:-}"
export NDNSF_DISABLE_NDNSD=1
export NDNSF_CONFIG="${tmpdir}/ndnsf.conf"
export NDNSF_SESSION_BASE="$(( $(date +%s) + $$ ))"
export NDN_LOG="${NDN_LOG:-ndn_service_framework.*=INFO}"

kill_stale_apps

if ! nfd-status >/dev/null 2>&1; then
  echo "Local NFD is not running or nfd-status is unavailable"
  echo "tmpdir=${tmpdir}"
  exit 1
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
  local deadline=$((SECONDS + 20))
  while (( SECONDS < deadline )); do
    if grep -q "Provider default registered service /HELLO" "${tmpdir}/provider.log" 2>/dev/null &&
       grep -q "Installed provider permission provider=/example/hello/provider service=/HELLO" "${tmpdir}/provider.log" 2>/dev/null; then
      return 0
    fi
    if [[ -n "${provider_pid}" ]] && ! kill -0 "${provider_pid}" 2>/dev/null; then
      return 1
    fi
    sleep 0.1
  done
  return 1
}

./build/examples/App_ServiceController >"${tmpdir}/controller.log" 2>&1 &
controller_pid=$!
if ! wait_for_controller; then
  echo "Controller did not become ready"
  echo "tmpdir=${tmpdir}"
  tail -n 120 "${tmpdir}/controller.log" 2>/dev/null || true
  exit 1
fi

./build/examples/App_Provider \
  --benchmark \
  --response-payload HELLO \
  --ack-payload "queue=0;gpu=idle" \
  >"${tmpdir}/provider.log" 2>&1 &
provider_pid=$!
if ! wait_for_provider; then
  echo "Provider did not become ready"
  echo "tmpdir=${tmpdir}"
  tail -n 160 "${tmpdir}/provider.log" 2>/dev/null || true
  exit 1
fi

benchmark_timeout_s=$(( ((count + warmup) * (timeout_ms + interval_ms) / 1000) + 60 ))
if (( benchmark_timeout_s < 90 )); then
  benchmark_timeout_s=90
fi

timeout "${benchmark_timeout_s}s" ./build/examples/App_User \
  --benchmark \
  --count "${count}" \
  --warmup "${warmup}" \
  --interval-ms "${interval_ms}" \
  --service /HELLO \
  --strategy "${strategy}" \
  --ack-timeout-ms "${ack_timeout_ms}" \
  --timeout-ms "${timeout_ms}" \
  --output-csv "${csv_path}" \
  >"${tmpdir}/user.log" 2>&1
user_status=$?

echo "tmpdir=${tmpdir}"
echo "csv=${csv_path}"
echo "user_status=${user_status}"
echo "controller_log=${tmpdir}/controller.log"
echo "provider_log=${tmpdir}/provider.log"
echo "user_log=${tmpdir}/user.log"
echo
echo "--- benchmark summary ---"
grep -E "^(count|success|timeout|avg_ms|min_ms|max_ms|p50_ms|p95_ms|p99_ms|strategy|csv)=|LOCAL_NFD_SERVICE_LATENCY_BENCHMARK=" "${tmpdir}/user.log" || true
echo
echo "--- provider tail ---"
tail -n 120 "${tmpdir}/provider.log" 2>/dev/null || true
echo
echo "--- user tail ---"
tail -n 160 "${tmpdir}/user.log" 2>/dev/null || true

if [[ "${user_status}" -eq 0 ]] &&
   grep -q "LOCAL_NFD_SERVICE_LATENCY_BENCHMARK=PASS" "${tmpdir}/user.log" &&
   grep -q "count=${count}" "${tmpdir}/user.log" &&
   grep -q "success=${count}" "${tmpdir}/user.log" &&
   grep -q "timeout=0" "${tmpdir}/user.log" &&
   [[ -s "${csv_path}" ]]; then
  exit 0
fi

echo
echo "LOCAL_NFD_SERVICE_LATENCY_BENCHMARK=FAIL"
exit 1
