#!/usr/bin/env bash
set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmpdir="$(mktemp -d /tmp/ndnsf-targeted-selection-confidentiality.XXXXXX)"

cd "${repo_root}"
export LD_LIBRARY_PATH="${repo_root}/build:${LD_LIBRARY_PATH:-}"

./build/unit-tests \
  --run_test=GenericDynamicApi/CryptoAndAuthorization \
  >"${tmpdir}/unit-tests.log" 2>&1
status=$?

echo "tmpdir=${tmpdir}"
echo "unit_test_status=${status}"
cat "${tmpdir}/unit-tests.log"

if [[ "${status}" -eq 0 ]] &&
   grep -q "No errors detected" "${tmpdir}/unit-tests.log"; then
  echo "TARGETED_SELECTION_CONFIDENTIALITY_REGRESSION=PASS"
  exit 0
fi

echo "TARGETED_SELECTION_CONFIDENTIALITY_REGRESSION=FAIL"
exit 1
