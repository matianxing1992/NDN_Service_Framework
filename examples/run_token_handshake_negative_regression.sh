#!/usr/bin/env bash
set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${NDNSF_BUILD_DIR:-${repo_root}/build}"
tmpdir="$(mktemp -d /tmp/ndnsf-token-handshake-negative.XXXXXX)"

cd "${repo_root}"
export LD_LIBRARY_PATH="${build_dir}:${LD_LIBRARY_PATH:-}"

"${build_dir}/unit-tests" \
  --run_test=GenericDynamicApi/TokensAndReplay/TokenHandshakeNegativeRegression \
  >"${tmpdir}/unit-tests.log" 2>&1
status=$?

echo "tmpdir=${tmpdir}"
echo "unit_test_status=${status}"
echo
cat "${tmpdir}/unit-tests.log"

if [[ "${status}" -eq 0 ]] &&
   grep -q "No errors detected" "${tmpdir}/unit-tests.log"; then
  echo
  echo "TOKEN_HANDSHAKE_NEGATIVE_REGRESSION=PASS"
  exit 0
fi

echo
echo "TOKEN_HANDSHAKE_NEGATIVE_REGRESSION=FAIL"
exit 1
