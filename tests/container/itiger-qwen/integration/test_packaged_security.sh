#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
runner="$repo/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-ndnsf-qwen.sh"
readiness="$repo/tests/container/itiger-qwen/integration/test_all_process_readiness.sh"
native="$repo/examples/DI_NativeProviderExecutable.cpp"
provider="$repo/ndn-service-framework/ServiceProvider.cpp"
user="$repo/ndn-service-framework/ServiceUser.cpp"

grep -q 'NDNSF_QWEN_REQUIRES_SLURM' "$runner"
grep -q 'NDNSF_DI_NATIVE_PROVIDER_READY' "$runner"
grep -q 'all_providers_ready' "$runner"
grep -q 'CONTROLLER_READINESS_TIMEOUT' "$runner"
grep -q 'USER_AFTER_ALL_READY' "$readiness"
grep -q 'NDNSF_DI_ORT_PROFILE_PREFIX' "$runner"
if grep -Eq 'isAuthorized[[:space:]]*=[[:space:]]*true|disable.*token|allowCpuFallback.*true' "$runner"; then
  exit 1
fi
grep -q 'providerToken' "$provider"
grep -q 'userToken' "$user"
grep -q 'executionEvidence' "$native"
grep -q 'NDNSF_DI_NATIVE_PROVIDER_READY' "$native"
grep -q 'NDNSF_DI_EXECUTION_EVIDENCE_UPDATE' "$native"

for regression in \
  run_hello_auth_regression.sh \
  run_nac_abe_attribute_routing_regression.sh \
  run_token_handshake_negative_regression.sh; do
  test -x "$repo/examples/$regression"
done
echo SPEC109_PACKAGED_SECURITY_CONTRACT_PASS
