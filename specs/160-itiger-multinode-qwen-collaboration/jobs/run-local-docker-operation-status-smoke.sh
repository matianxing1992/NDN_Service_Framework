#!/bin/bash
set -Eeuo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
jobs_dir="$repo_root/specs/160-itiger-multinode-qwen-collaboration/jobs"
image=${SPEC160_LOCAL_IMAGE:-ndnsf-di:spec158-spec160-operation-status-binding-20260728T052052Z-e}
run_id=${SPEC160_LOCAL_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}
result_dir="$repo_root/results/spec160-itiger-multinode-qwen/local-docker-operation-status-$run_id"

test ! -e "$result_dir"
mkdir -p "$result_dir/home"

docker image inspect "$image" > "$result_dir/image-inspect.json"
printf '%s\n' \
  'memory=4g' \
  'memory-swap=5g' \
  'cpus=2' \
  'runtime=fake' \
  'stages=1' \
  'model-load=disabled' \
  > "$result_dir/container-limits.txt"
docker run --rm \
  --name "spec160-operation-status-${run_id,,}" \
  --memory=4g \
  --memory-swap=5g \
  --cpus=2 \
  --user 0:0 \
  --read-only \
  --tmpfs /tmp:rw,nosuid,nodev,size=256m \
  --mount "type=bind,src=$result_dir,dst=/scratch" \
  --mount "type=bind,src=$jobs_dir,dst=/harness,readonly" \
  --mount "type=bind,src=$repo_root/examples/python/NDNSF-DistributedInference/llm_pipeline,dst=/source/llm_pipeline,readonly" \
  --entrypoint /bin/bash \
  "$image" /harness/local-docker-operation-status-inner.sh \
  2>&1 | tee "$result_dir/smoke.log"

(
  cd "$result_dir"
  find . -maxdepth 1 -type f ! -name checksums.sha256 -print0 |
    sort -z |
    xargs -0 sha256sum
) > "$result_dir/checksums.sha256"

printf 'SPEC160_LOCAL_DOCKER_EVIDENCE=%s\n' "$result_dir"
