#!/usr/bin/env bash
set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmpdir="$(mktemp -d /tmp/ndnsf-secure-selection-status.XXXXXX)"

cd "${repo_root}"
export LD_LIBRARY_PATH="${repo_root}/build:${LD_LIBRARY_PATH:-}"
export PYTHONPATH="${repo_root}/pythonWrapper:${repo_root}/NDNSF-DistributedInference${PYTHONPATH:+:${PYTHONPATH}}"

./build/unit-tests \
  --run_test=GenericDynamicApi/CollaborationStatus \
  >"${tmpdir}/cpp.log" 2>&1
cpp_status=$?
python3 -m unittest discover -s tests/python -p 'test_spec129_secure_status.py' \
  >"${tmpdir}/python.log" 2>&1
python_status=$?

echo "tmpdir=${tmpdir}"
echo "cpp_status=${cpp_status}"
echo "python_status=${python_status}"
cat "${tmpdir}/cpp.log"
cat "${tmpdir}/python.log"

if [[ "${cpp_status}" -eq 0 && "${python_status}" -eq 0 ]] &&
   grep -q "No errors detected" "${tmpdir}/cpp.log" &&
   grep -q "OK" "${tmpdir}/python.log"; then
  echo "SECURE_SELECTION_STATUS_REGRESSION=PASS"
  exit 0
fi

echo "SECURE_SELECTION_STATUS_REGRESSION=FAIL"
exit 1
