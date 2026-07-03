#!/usr/bin/env bash

ndnsf_start_nfd_if_needed() {
  local log_file="$1"
  NDNSF_COMMON_NFD_STARTED="false"
  if [[ ! -S /run/nfd/nfd.sock ]]; then
    nfd-start >"${log_file}" 2>&1
    NDNSF_COMMON_NFD_STARTED="true"
    local deadline=$((SECONDS + 10))
    while (( SECONDS < deadline )); do
      [[ -S /run/nfd/nfd.sock ]] && return 0
      sleep 0.1
    done
  fi
  [[ -S /run/nfd/nfd.sock ]]
}

ndnsf_stop_nfd_if_started() {
  if [[ "${NDNSF_COMMON_NFD_STARTED:-false}" == "true" ]]; then
    nfd-stop >/dev/null 2>&1 || true
  fi
}

ndnsf_wait_for_log() {
  local log_file="$1"
  local pattern="$2"
  local timeout_s="${3:-15}"
  local deadline=$((SECONDS + timeout_s))
  while (( SECONDS < deadline )); do
    if grep -Eq "${pattern}" "${log_file}" 2>/dev/null; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

ndnsf_dump_tail() {
  local title="$1"
  local file="$2"
  local lines="${3:-120}"
  echo
  echo "--- ${title} ---"
  if [[ -f "${file}" ]]; then
    tail -n "${lines}" "${file}"
  else
    echo "<missing ${file}>"
  fi
}
