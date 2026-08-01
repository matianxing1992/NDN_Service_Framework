#!/usr/bin/env bash
set -euo pipefail

repo=/workspace
evidence=/evidence
runtime=/tmp/spec163-placement-preparation
mkdir -p "${evidence}" "${runtime}/home/.ndn" "${runtime}/state"

export HOME="${runtime}/home"
export PYTHONDONTWRITEBYTECODE=1
export PYTHONPATH="${repo}/pythonWrapper:${repo}/NDNSF-DistributedInference:${repo}/NDNSF-DistributedRepo/pythonWrapper"
export LD_LIBRARY_PATH="${repo}/build:/hostlocal:/hostlib:/hostonnx/lib:/opt/ndn-base/lib:/opt/ndnsf-app/lib:/opt/ndnsf/lib"
export NDNSF_DISABLE_NDNSD=1
export NDNSF_CONFIG="${runtime}/ndnsf.conf"
export NDN_LOG="ndn_service_framework.*=INFO"
export PATH="/opt/ndn-base/bin:/opt/ndnsf-app/bin:${PATH}"

cd "${repo}"

memory_max="$(cat /sys/fs/cgroup/memory.max 2>/dev/null || cat /sys/fs/cgroup/memory/memory.limit_in_bytes)"
swap_max="$(cat /sys/fs/cgroup/memory.swap.max 2>/dev/null || true)"
{
  echo "memory.max=${memory_max}"
  echo "memory.swap.max=${swap_max:-unavailable}"
  echo "torch_processes_before=$(pgrep -af 'torch|qwen' | wc -l)"
} >"${evidence}/resource-boundary.txt"

sed "s#/run/nfd/nfd.sock#${runtime}/nfd.sock#g" \
  /etc/ndn/nfd.conf >"${runtime}/nfd.conf"
echo "transport=unix://${runtime}/nfd.sock" >"${HOME}/.ndn/client.conf"
nfd --config "${runtime}/nfd.conf" >"${evidence}/nfd.log" 2>&1 &
nfd_pid=$!
cleanup() {
  kill "${nfd_pid:-}" >/dev/null 2>&1 || true
  wait "${nfd_pid:-}" >/dev/null 2>&1 || true
}
trap cleanup EXIT
for _ in $(seq 1 50); do
  [[ -S "${runtime}/nfd.sock" ]] && break
  sleep 0.1
done
[[ -S "${runtime}/nfd.sock" ]]

run_gate() {
  local name="$1"
  shift
  echo "RUN ${name}" | tee -a "${evidence}/gate-summary.txt"
  if timeout 90s "$@" >"${evidence}/${name}.log" 2>&1; then
    echo "PASS ${name}" | tee -a "${evidence}/gate-summary.txt"
  else
    status=$?
    echo "FAIL ${name} status=${status}" | tee -a "${evidence}/gate-summary.txt"
    tail -n 160 "${evidence}/${name}.log" || true
    return "${status}"
  fi
}

run_fake_di_lifecycle() {
  local name=fake-di-lifecycle
  local work="${runtime}/fake"
  mkdir -p "${work}"
  export NDNSF_SESSION_BASE="$(( $(date +%s) + $$ ))"
  nfdc strategy set /example/hello/group \
    /localhost/nfd/strategy/multicast/v=5 >/dev/null 2>&1 || true
  "${repo}/build/examples/App_ServiceController" \
    >"${work}/controller.log" 2>&1 &
  controller_pid=$!
  provider_pids=()
  local cleanup_apps
  cleanup_apps() {
    for pid in "${provider_pids[@]:-}" "${controller_pid:-}"; do
      if [[ -n "${pid}" ]]; then
        kill "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
      fi
    done
  }
  fail_fake_lifecycle() {
    local message="$1"
    echo "SPEC163_FAKE_LIFECYCLE_FAILURE ${message}" >&2
    {
      cat "${work}/controller.log" 2>/dev/null || true
      cat "${work}/provider-A.log" 2>/dev/null || true
      cat "${work}/provider-B.log" 2>/dev/null || true
      cat "${work}/provider-C.log" 2>/dev/null || true
      cat "${work}/user.log" 2>/dev/null || true
    } >"${evidence}/${name}.log"
    cleanup_apps
    trap cleanup EXIT
    return 1
  }
  trap 'cleanup_apps; cleanup' EXIT
  for _ in $(seq 1 100); do
    grep -q "ServiceController listening on:" "${work}/controller.log" && break
    sleep 0.1
  done
  grep -q "ServiceController listening on:" "${work}/controller.log" ||
    fail_fake_lifecycle "controller-not-ready" || return 1
  for provider_id in A B C; do
    /usr/bin/python3 \
      "${repo}/tests/container/placement-preparation/fake_provider.py" \
      --provider-id "${provider_id}" --state-dir "${runtime}/state" \
      >"${work}/provider-${provider_id}.log" 2>&1 &
    provider_pids+=("$!")
    # Initialize each KeyChain serially.  They subsequently remain live
    # together, but do not race while opening the shared PIB database.
    for _ in $(seq 1 150); do
      grep -q "SPEC163_PROVIDER_READY" \
        "${work}/provider-${provider_id}.log" && break
      if ! kill -0 "${provider_pids[-1]}" 2>/dev/null; then
        break
      fi
      sleep 0.1
    done
    grep -q "SPEC163_PROVIDER_READY" "${work}/provider-${provider_id}.log" ||
      fail_fake_lifecycle "provider-${provider_id}-not-ready" || return 1
  done
  timeout 45s /usr/bin/python3 \
    "${repo}/tests/container/placement-preparation/fake_user.py" \
    >"${work}/user.log" 2>&1 ||
    fail_fake_lifecycle "user-invocation-failed" || return 1
  sleep 1
  grep -q "SPEC163_SECURE_DEFERRED_LIFECYCLE_OK" "${work}/user.log" ||
    fail_fake_lifecycle "missing-secure-success" || return 1
  grep -q "roles=left,merge" "${work}/user.log" ||
    fail_fake_lifecycle "missing-multi-role-assignment" || return 1
  grep -q "SPEC163_LOCAL_READY" "${work}/provider-B.log" ||
    fail_fake_lifecycle "missing-provider-readiness" || return 1
  marker_ts() {
    local file="$1"
    local marker="$2"
    local qualifier="${3:-}"
    awk -v marker="${marker}" -v qualifier="${qualifier}" '
      index($0, marker) && (qualifier == "" || index($0, qualifier)) {
        for (i = 1; i <= NF; ++i) {
          if ($i ~ /^tsNs=/) {
            sub(/^tsNs=/, "", $i)
            print $i
            exit
          }
        }
      }' "${file}"
  }
  commit_ts="$(marker_ts "${work}/user.log" SPEC163_PLAN_COMMIT)"
  source_start_ts="$(marker_ts "${work}/provider-A.log" \
    SPEC163_ROLE_START role=source)"
  source_output_ts="$(marker_ts "${work}/provider-A.log" \
    SPEC163_OUTPUT_PUBLISHED role=source)"
  right_input_ts="$(marker_ts "${work}/provider-C.log" \
    SPEC163_INPUT_ARRIVED role=right)"
  right_ready_ts="$(marker_ts "${work}/provider-C.log" \
    SPEC163_LOCAL_READY role=right)"
  right_start_ts="$(marker_ts "${work}/provider-C.log" \
    SPEC163_ROLE_START role=right)"
  left_ready_ts="$(marker_ts "${work}/provider-B.log" \
    SPEC163_LOCAL_READY role=left)"
  left_input_ts="$(marker_ts "${work}/provider-B.log" \
    SPEC163_INPUT_ARRIVED role=left)"
  merge_input_ts="$(marker_ts "${work}/provider-B.log" \
    SPEC163_INPUT_ARRIVED role=merge)"
  merge_start_ts="$(marker_ts "${work}/provider-B.log" \
    SPEC163_ROLE_START role=merge)"
  for value in "${commit_ts}" "${source_start_ts}" "${source_output_ts}" \
      "${right_input_ts}" "${right_ready_ts}" "${right_start_ts}" \
      "${left_ready_ts}" "${left_input_ts}" "${merge_input_ts}" \
      "${merge_start_ts}"; do
    [[ "${value}" =~ ^[0-9]+$ ]] ||
      fail_fake_lifecycle "missing-timeline-marker" || return 1
  done
  (( commit_ts < right_ready_ts )) ||
    fail_fake_lifecycle "Selection-did-not-precede-complete-readiness" ||
    return 1
  (( source_start_ts < right_ready_ts && source_output_ts < right_ready_ts )) ||
    fail_fake_lifecycle "source-did-not-progress-during-downstream-prepare" ||
    return 1
  (( right_input_ts < right_ready_ts && right_ready_ts <= right_start_ts )) ||
    fail_fake_lifecycle "input-before-model-latch-order-invalid" || return 1
  (( left_ready_ts < left_input_ts )) ||
    fail_fake_lifecycle "model-before-input-latch-order-invalid" || return 1
  (( merge_input_ts <= merge_start_ts )) ||
    fail_fake_lifecycle "fanin-did-not-trigger-merge" || return 1
  if grep -qE "Traceback|Segmentation fault|core dumped" \
      "${work}"/*.log; then
    fail_fake_lifecycle "process-crash-or-traceback" || return 1
  fi
  {
    cat "${work}/controller.log"
    cat "${work}/provider-A.log"
    cat "${work}/provider-B.log"
    cat "${work}/provider-C.log"
    cat "${work}/user.log"
  } >"${evidence}/${name}.log"
  cleanup_apps
  trap cleanup EXIT
}

run_gate selective-ack-3-provider \
  "${repo}/examples/run_selective_ack_custom_selection_regression.sh"
run_gate hello-auth "${repo}/examples/run_hello_auth_regression.sh"
run_gate nac-abe-routing \
  env 'NDN_LOG=ndn_service_framework.*=DEBUG' \
  "${repo}/examples/run_nac_abe_attribute_routing_regression.sh"
run_gate token-negative \
  "${repo}/examples/run_token_handshake_negative_regression.sh"

echo "RUN fake-di-lifecycle" | tee -a "${evidence}/gate-summary.txt"
if run_fake_di_lifecycle; then
  echo "PASS fake-di-lifecycle" | tee -a "${evidence}/gate-summary.txt"
else
  echo "FAIL fake-di-lifecycle" | tee -a "${evidence}/gate-summary.txt"
  exit 1
fi

run_gate encrypted-permission-unit "${repo}/build/unit-tests" \
  --run_test=EncryptedPermissionResponse \
  --log_level=test_suite
run_gate crypto-authorization-unit "${repo}/build/unit-tests" \
  --run_test=GenericDynamicApi/CryptoAndAuthorization \
  --log_level=test_suite
run_gate tokens-replay-unit "${repo}/build/unit-tests" \
  --run_test=GenericDynamicApi/TokensAndReplay \
  --log_level=test_suite
run_gate opaque-selection-unit "${repo}/build/unit-tests" \
  --run_test=GenericOpaqueSelection \
  --log_level=test_suite
run_gate collaboration-status-unit "${repo}/build/unit-tests" \
  --run_test=GenericDynamicApi/CollaborationStatus \
  --log_level=test_suite

for test in \
  test_ndnsf_di_selection_dataflow.py \
  test_ndnsf_di_dependency_dag.py \
  test_ndnsf_di_compensation.py \
  test_ndnsf_di_lifecycle_history.py \
  test_ndnsf_di_core_ownership.py \
  test_ndnsf_deferred_collaboration.py \
  test_ndnsf_di_model_family_adapter.py \
  test_ndnsf_di_model_adapters.py \
  test_ndnsf_di_core_state.py \
  test_ndnsf_di_automatic_collaboration_plan.py \
  test_ndnsf_di_presplit_first_strategy.py; do
  run_gate "${test%.py}" env \
    "PYTHONPATH=${repo}/pythonWrapper:${repo}/NDNSF-DistributedInference:${repo}/NDNSF-DistributedRepo/pythonWrapper:/hostpy" \
    /usr/bin/python3 \
    "${repo}/tests/python/${test}" -q
done

{
  echo "torch_processes_after=$(pgrep -af 'torch|qwen' | wc -l)"
  echo "nfd_processes=$(pgrep -x nfd | wc -l)"
  echo "controller_count_required=1"
  echo "provider_count_required=3"
  echo "payload_kind=byte-sized-fake"
  echo "model_loaded=false"
} >>"${evidence}/resource-boundary.txt"

grep -q '^FAIL ' "${evidence}/gate-summary.txt" && exit 1
echo "SPEC163_LOCAL_DOCKER_ALL_GATES_PASS" | tee -a "${evidence}/gate-summary.txt"
