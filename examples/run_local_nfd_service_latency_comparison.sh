#!/usr/bin/env bash
set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmpdir="$(mktemp -d /tmp/ndnsf-local-nfd-latency-comparison.XXXXXX)"

count="${NDNSF_COMPARISON_COUNT:-100}"
warmup="${NDNSF_COMPARISON_WARMUP:-10}"
timeout_ms="${NDNSF_COMPARISON_TIMEOUT_MS:-20000}"
interval_ms="${NDNSF_COMPARISON_INTERVAL_MS:-1000}"

controller_pid=""
provider_pid=""
comparison_failed=0
result_rows=()

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

metric() {
  local log="$1"
  local key="$2"
  awk -F= -v key="${key}" '$1 == key { value = $2 } END { print value }' "${log}" 2>/dev/null
}

run_benchmark() {
  local label="$1"
  local strategy="$2"
  local ack_timeout_ms="$3"
  local csv_path="${tmpdir}/${label}.csv"
  local user_log="${tmpdir}/user-${label}.log"
  local display_ack="${ack_timeout_ms}"

  if [[ "${display_ack}" == "0" ]]; then
    display_ack="-"
  fi

  echo
  echo "=== ${label} ==="
  echo "strategy=${strategy} ack_timeout_ms=${display_ack} csv=${csv_path}"

  local benchmark_timeout_s=$(( ((count + warmup) * (timeout_ms + interval_ms) / 1000) + 60 ))
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
    >"${user_log}" 2>&1
  local user_status=$?

  grep -E "^(count|success|timeout|avg_ms|min_ms|max_ms|p50_ms|p95_ms|p99_ms|strategy|csv)=|LOCAL_NFD_SERVICE_LATENCY_BENCHMARK=" "${user_log}" || true

  local pass="FAIL"
  if [[ "${user_status}" -eq 0 ]] &&
     grep -q "LOCAL_NFD_SERVICE_LATENCY_BENCHMARK=PASS" "${user_log}" &&
     grep -q "count=${count}" "${user_log}" &&
     grep -q "success=${count}" "${user_log}" &&
     grep -q "timeout=0" "${user_log}" &&
     [[ -s "${csv_path}" ]]; then
    pass="PASS"
  else
    comparison_failed=1
    echo "--- ${label} user tail ---"
    tail -n 180 "${user_log}" 2>/dev/null || true
  fi

  result_rows+=("${label}|${display_ack}|${pass}|$(metric "${user_log}" count)|$(metric "${user_log}" success)|$(metric "${user_log}" timeout)|$(metric "${user_log}" avg_ms)|$(metric "${user_log}" min_ms)|$(metric "${user_log}" max_ms)|$(metric "${user_log}" p50_ms)|$(metric "${user_log}" p95_ms)|$(metric "${user_log}" p99_ms)|${csv_path}")

  sleep 1
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

echo "tmpdir=${tmpdir}"
echo "controller_log=${tmpdir}/controller.log"
echo "provider_log=${tmpdir}/provider.log"
echo "warmup=${warmup}"
echo "count=${count}"

run_benchmark "all-selected" "all-selected" "0"
run_benchmark "first-responding" "first-responding" "0"
run_benchmark "custom-selection-ack50" "custom-selection" "50"
run_benchmark "custom-selection-ack100" "custom-selection" "100"
run_benchmark "custom-selection-ack200" "custom-selection" "200"
run_benchmark "custom-selection-ack500" "custom-selection" "500"

echo
echo "=== comparison table ==="
printf "%-26s %8s %6s %6s %7s %7s %10s %10s %10s %10s %10s %10s %s\n" \
  "mode" "ack_ms" "pass" "count" "success" "timeout" \
  "avg_ms" "min_ms" "max_ms" "p50_ms" "p95_ms" "p99_ms" "csv"
for row in "${result_rows[@]}"; do
  IFS='|' read -r label ack pass row_count success timeout_count avg min max p50 p95 p99 csv <<<"${row}"
  printf "%-26s %8s %6s %6s %7s %7s %10s %10s %10s %10s %10s %10s %s\n" \
    "${label}" "${ack}" "${pass}" "${row_count}" "${success}" "${timeout_count}" \
    "${avg}" "${min}" "${max}" "${p50}" "${p95}" "${p99}" "${csv}"
done

if [[ "${comparison_failed}" -eq 0 ]]; then
  echo
  echo "LOCAL_NFD_SERVICE_LATENCY_COMPARISON=PASS"
  exit 0
fi

echo
echo "LOCAL_NFD_SERVICE_LATENCY_COMPARISON=FAIL"
exit 1
