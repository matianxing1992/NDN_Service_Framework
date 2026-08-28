#!/bin/bash
set -Eeuo pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
readonly JOBS="$ROOT/specs/167-itiger-repo-throughput/jobs"
readonly IMAGE="${SPEC167_IMAGE:-ndnsf-di:spec165-minindn-gate}"
readonly SOURCE_ROOT="${SPEC167_SOURCE_ROOT:-$ROOT}"
readonly TOKEN="spec167-local-$RANDOM-$$"
readonly NETWORK="${TOKEN}-net"
readonly C0="${TOKEN}-rank0"
readonly C1="${TOKEN}-rank1"
readonly WORK="$(mktemp -d /tmp/spec167-two-container.XXXXXX)"

cleanup()
{
  rc=$?
  trap - EXIT INT TERM
  if test "$rc" -ne 0; then
    echo "SPEC167_LOCAL_TWO_CONTAINER_PREFLIGHT_FAIL rc=$rc" >&2
    docker logs "$C0" 2>&1 | tail -n 160 >&2 || true
    docker logs "$C1" 2>&1 | tail -n 160 >&2 || true
  fi
  docker rm -f "$C0" "$C1" >/dev/null 2>&1 || true
  docker network rm "$NETWORK" >/dev/null 2>&1 || true
  rm -rf -- "$WORK"
  exit "$rc"
}
trap cleanup EXIT INT TERM

mkdir -p "$WORK/shared/node-0" "$WORK/shared/node-1" "$WORK/source"
(
  cd "$SOURCE_ROOT"
  rsync -a --relative --exclude='__pycache__' --exclude='*.pyc' \
    Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py \
    Experiments/NDNSF_DistributedRepo_Artifact_Itiger.py \
    Experiments/analyze_spec167_itiger_repo.py \
    Experiments/spec164_artifact_campaign.py \
    specs/167-itiger-repo-throughput \
    "$WORK/source/"
)
for rank in 0 1; do
  scratch="$WORK/rank-${rank}"
  mkdir -p "$scratch/run" "$scratch/log" "$scratch/data" "$scratch/home"
  sed -e 's|@@NFD_SOCKET@@|/scratch/run/nfd.sock|g' \
      -e 's|@@TCP_PORT@@|26368|g' \
      -e "s|@@NODE_RANK@@|${rank}|g" \
      "$JOBS/nfd.conf.in" > "$scratch/nfd.conf"
done
printf '%s\n' "$C0" > "$WORK/shared/node-0/ipv4.txt"
printf '%s\n' "$C1" > "$WORK/shared/node-1/ipv4.txt"

docker network create "$NETWORK" >/dev/null
run_rank()
{
  local rank=$1 name=$2
  docker run -d --name "$name" --network "$NETWORK" \
    --user "$(id -u):$(id -g)" \
    --cap-drop ALL --security-opt no-new-privileges \
    -e HOME=/scratch/home \
    -e "SPEC167_RANK=${rank}" -e SPEC167_PORT=26368 \
    -e SPEC167_MODE=preflight \
    -v "$WORK/rank-${rank}:/scratch:rw" \
    -v "$WORK/shared:/shared:rw" \
    -v "$WORK/source:/source:ro" \
    --entrypoint /source/specs/167-itiger-repo-throughput/jobs/rank-inner.sh \
    "$IMAGE" >/dev/null
}

run_rank 0 "$C0"
run_rank 1 "$C1"
timeout 360s docker wait "$C0" >/dev/null
timeout 360s docker wait "$C1" >/dev/null
docker logs "$C0" > "$WORK/rank-0/container.log" 2>&1 || true
docker logs "$C1" > "$WORK/rank-1/container.log" 2>&1 || true
test "$(docker inspect -f '{{.State.ExitCode}}' "$C0")" -eq 0
test "$(docker inspect -f '{{.State.ExitCode}}' "$C1")" -eq 0
test -f "$WORK/shared/rank-complete-0"
test -f "$WORK/shared/rank-complete-1"
test -f "$WORK/shared/preflight/preflight.json"
test -f "$WORK/shared/preflight/preflight.cold.json"
test ! -e "$WORK/shared/preflight/payload.bin"
python3 - "$WORK/shared/preflight" <<'PY'
import json,pathlib,sys
root=pathlib.Path(sys.argv[1])
result=json.loads((root/'preflight.json').read_text())
cold=json.loads((root/'preflight.cold.json').read_text())
assert result['status']=='SUCCESS' and cold['status']=='SUCCESS'
assert cold['destination'].startswith('/scratch/data/')
print('SPEC167_LOCAL_TWO_CONTAINER_PREFLIGHT_PASS')
PY
