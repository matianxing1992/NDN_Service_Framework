#!/bin/bash
set -Eeuo pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
readonly JOBS="$ROOT/specs/167-itiger-repo-throughput/jobs"
readonly IMAGE="${SPEC167_IMAGE:-ndnsf-di:spec165-minindn-gate}"
readonly SOURCE_ROOT="${SPEC167_SOURCE_ROOT:-$ROOT}"
readonly TOKEN="spec167-formal-local-$RANDOM-$$"
readonly NETWORK="${TOKEN}-net"
readonly C0="${TOKEN}-rank0"
readonly C1="${TOKEN}-rank1"
readonly SUBJECT="${SPEC167_LOCAL_SUBJECT:-raw-segmented-ndn}"
readonly RUN_NAME="formal-${SUBJECT}-64m-smoke"
readonly WORK="$(mktemp -d /tmp/spec167-formal-two-container.XXXXXX)"

cleanup()
{
  rc=$?
  trap - EXIT INT TERM
  if test "$rc" -ne 0; then
    echo "SPEC167_LOCAL_FORMAL_SMOKE_FAIL rc=$rc" >&2
    docker logs "$C0" 2>&1 | tail -n 160 >&2 || true
    docker logs "$C1" 2>&1 | tail -n 160 >&2 || true
    for rank in 0 1; do
      for log in "$WORK/rank-${rank}"/log/*; do
        test -f "$log" || continue
        echo "--- rank-${rank}/$(basename "$log")" >&2
        tail -n 80 "$log" >&2 || true
      done
    done
  fi
  docker rm -f "$C0" "$C1" >/dev/null 2>&1 || true
  docker network rm "$NETWORK" >/dev/null 2>&1 || true
  if test "$rc" -eq 0; then
    rm -rf -- "$WORK"
  else
    echo "SPEC167_LOCAL_FORMAL_SMOKE_WORK=$WORK" >&2
  fi
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
      -e 's|@@TCP_PORT@@|26378|g' \
      -e "s|@@NODE_RANK@@|${rank}|g" \
      "$JOBS/nfd.conf.in" > "$scratch/nfd.conf"
done
printf '%s\n' "$C0" > "$WORK/shared/node-0/ipv4.txt"
printf '%s\n' "$C1" > "$WORK/shared/node-1/ipv4.txt"

for _ in $(seq 1 20); do
  SUBNET="10.254.$((20 + RANDOM % 220)).0/24"
  docker network create --subnet "$SUBNET" "$NETWORK" >/dev/null 2>&1 && break
done
docker network inspect "$NETWORK" >/dev/null
read -r IP0 IP1 < <(python3 - "$SUBNET" <<'PY'
import ipaddress, sys
network=ipaddress.ip_network(sys.argv[1])
print(network.network_address + 10, network.network_address + 11)
PY
)
run_rank()
{
  local rank=$1 name=$2
  local own_ip peer_ip
  if test "$rank" -eq 0; then own_ip=$IP0; peer_ip=$IP1; else own_ip=$IP1; peer_ip=$IP0; fi
  docker run -d --name "$name" --network "$NETWORK" \
    --ip "$own_ip" \
    --user "$(id -u):$(id -g)" \
    --cap-drop ALL --security-opt no-new-privileges \
    -e HOME=/scratch/home \
    -e "SPEC167_RANK=${rank}" -e SPEC167_PORT=26378 \
    -e "SPEC167_PEER_HOST=${peer_ip}" \
    -e "SPEC167_STARTUP_DELAY_SECONDS=$([ "$rank" -eq 1 ] && echo 1 || echo 0)" \
    -e SPEC167_MODE=formal -e "SPEC167_RUN_NAME=$RUN_NAME" \
    -e "SPEC167_SUBJECT=$SUBJECT" -e SPEC167_PAYLOAD_SIZE=67108864 \
    -e SPEC167_TCP_PORT=26379 -e SPEC167_DURATION_SECONDS=1 \
    -v "$WORK/rank-${rank}:/scratch:rw" \
    -v "$WORK/shared:/shared:rw" \
    -v "$WORK/source:/source:ro" \
    --entrypoint /source/specs/167-itiger-repo-throughput/jobs/campaign-rank-inner.sh \
    "$IMAGE" >/dev/null
}

run_rank 0 "$C0"
run_rank 1 "$C1"
timeout 360s docker wait "$C0" >/dev/null
timeout 360s docker wait "$C1" >/dev/null
test "$(docker inspect -f '{{.State.ExitCode}}' "$C0")" -eq 0
test "$(docker inspect -f '{{.State.ExitCode}}' "$C1")" -eq 0
readonly RESULT="$WORK/shared/runs/$RUN_NAME"
test -f "$RESULT/rank-complete-0"
test -f "$RESULT/rank-complete-1"
test -f "$RESULT/result.json"
test -f "$RESULT/result.cold.json"
if test "$SUBJECT" = digest-only -o "$SUBJECT" = signed-manifest; then
  test -f "$RESULT/result.warm.json"
fi
test ! -e "$RESULT/payload.bin"
python3 - "$RESULT" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
forward = json.loads((root / "result.json").read_text())
cold = json.loads((root / "result.cold.json").read_text())
assert forward["status"] == "SUCCESS"
assert cold["status"] == "SUCCESS"
assert forward["logicalBytes"] == 67108864
assert cold["logicalBytes"] == 67108864
assert cold["destination"].startswith("/scratch/data/")
print("SPEC167_LOCAL_FORMAL_SMOKE_PASS")
PY
