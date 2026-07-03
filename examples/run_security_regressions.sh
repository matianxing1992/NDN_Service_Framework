#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

nfd_started="false"

cleanup() {
  if [[ "${nfd_started}" == "true" ]]; then
    nfd-stop >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

if [[ ! -S /run/nfd/nfd.sock ]]; then
  nfd-start >/tmp/ndnsf-security-regressions-nfd.log 2>&1
  nfd_started="true"
  deadline=$((SECONDS + 10))
  while (( SECONDS < deadline )); do
    if [[ -S /run/nfd/nfd.sock ]]; then
      break
    fi
    sleep 0.1
  done
fi

if [[ ! -S /run/nfd/nfd.sock ]]; then
  echo "NFD socket /run/nfd/nfd.sock is unavailable" >&2
  if [[ -f /tmp/ndnsf-security-regressions-nfd.log ]]; then
    tail -n 80 /tmp/ndnsf-security-regressions-nfd.log >&2
  fi
  exit 1
fi

regressions=(
  "examples/run_hello_auth_regression.sh"
  "examples/run_hello_ack_payload_regression.sh"
  "examples/run_selective_ack_custom_selection_regression.sh"
  "examples/run_nac_abe_attribute_routing_regression.sh"
  "examples/run_token_handshake_negative_regression.sh"
  "examples/run_token_certificate_bootstrap_regression.sh"
)

for regression in "${regressions[@]}"; do
  echo "=== ${regression} ==="
  "${regression}"
done

echo "NDNSF_SECURITY_REGRESSIONS=PASS"
