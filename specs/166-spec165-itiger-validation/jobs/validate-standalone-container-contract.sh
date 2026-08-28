#!/bin/bash
set -Eeuo pipefail

readonly REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
readonly JOB="${REPO_ROOT}/specs/166-spec165-itiger-validation/jobs/standalone-gpu-reference.sbatch"
readonly IMAGE='ndnsf-di:spec165-minindn-gate'
readonly PIPELINE="${REPO_ROOT}/examples/python/NDNSF-DistributedInference/llm_pipeline"

python_path=$(
  sed -n 's/^[[:space:]]*--env PYTHONPATH=\([^[:space:]]*\).*/\1/p' "$JOB"
)
interpreter=$(
  sed -n 's|^[[:space:]]*\(/opt/venv/bin/python\) /source/run_standalone_gpu_reference.py.*|\1|p' "$JOB"
)
test "$python_path" = '/source/llm_pipeline:/opt/ndnsf-app/python'
test "$interpreter" = '/opt/venv/bin/python'

docker run --rm --network none --read-only \
  --tmpfs /tmp:rw,noexec,nosuid,size=64m \
  -e "PYTHONPATH=${python_path}" \
  -v "${PIPELINE}:/source/llm_pipeline:ro" \
  --entrypoint "$interpreter" \
  "$IMAGE" \
  -c 'import ndnsf_distributed_inference, llm_pipeline_lib, provider'

echo SPEC166_EXACT_SBATCH_ENV_IMPORT_PASS
