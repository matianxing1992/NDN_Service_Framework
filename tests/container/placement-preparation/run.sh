#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
image="${NDNSF_SPEC163_IMAGE:-ndnsf-di:spec162-qwen36-runtime-aedbff59-20260728b}"
output="${1:-${repo_root}/results/spec163-local-docker-$(date +%Y%m%d_%H%M%S)}"
mkdir -p "${output}"
output="$(cd "${output}" && pwd)"
cidfile="$(mktemp /tmp/spec163-container.XXXXXX)"
rm -f "${cidfile}"
container_name="ndnsf-spec163-$RANDOM-$$"

cleanup() {
  docker rm -f "${container_name}" >/dev/null 2>&1 || true
  rm -f "${cidfile}"
}
trap cleanup EXIT

set +e
docker run \
  --name "${container_name}" \
  --cidfile "${cidfile}" \
  --memory=4g \
  --memory-swap=5g \
  --pids-limit=1024 \
  --user "$(id -u):$(id -g)" \
  --network=bridge \
  --entrypoint /bin/bash \
  --workdir /workspace \
  --volume "${repo_root}:/workspace:ro" \
  --volume "${output}:/evidence:rw" \
  --volume /lib/x86_64-linux-gnu:/hostlib:ro \
  --volume /usr/local/lib:/hostlocal:ro \
  --volume /opt/onnxruntime:/hostonnx:ro \
  --volume /usr/lib/python3/dist-packages:/hostpy:ro \
  "${image}" \
  /workspace/tests/container/placement-preparation/container-entrypoint.sh
status=$?
set -e

docker inspect "${container_name}" \
  --format 'image={{.Config.Image}} memory={{.HostConfig.Memory}} memorySwap={{.HostConfig.MemorySwap}} pidsLimit={{.HostConfig.PidsLimit}} oomKilled={{.State.OOMKilled}} exitCode={{.State.ExitCode}}' \
  >"${output}/docker-inspect.txt"
echo "evidence=${output}"
echo "status=${status}"
exit "${status}"
