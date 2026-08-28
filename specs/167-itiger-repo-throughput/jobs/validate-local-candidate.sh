#!/bin/bash
set -Eeuo pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
readonly IMAGE="${SPEC167_IMAGE:-ndnsf-di:spec165-minindn-gate}"
readonly JOBS="$ROOT/specs/167-itiger-repo-throughput/jobs"
readonly SOURCE_ROOT="${SPEC167_SOURCE_ROOT:-$ROOT}"

test -x "$JOBS/validate-local-candidate.sh"
test -x "$JOBS/rank.sh"
test -x "$JOBS/rank-inner.sh"
test -x "$JOBS/campaign-rank-inner.sh"
test -x "$JOBS/run-local-two-container-formal-smoke.sh"
test -f "$JOBS/nfd.conf.in"
test -f "$SOURCE_ROOT/Experiments/NDNSF_DistributedRepo_Artifact_Itiger.py"
test -f "$SOURCE_ROOT/Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py"
test -f "$SOURCE_ROOT/Experiments/analyze_spec167_itiger_repo.py"

if test -f "$SOURCE_ROOT/source-manifest.json"; then
  python3 "$ROOT/Experiments/validate_spec167_source_bundle.py" \
    --source-root "$SOURCE_ROOT" \
    --manifest "$SOURCE_ROOT/source-manifest.json"
else
  test -f "$SOURCE_ROOT/Experiments/spec164_artifact_campaign.py"
fi

# The sealed image runs as an unprivileged UID.  A staging directory that is
# only traversable by the login user passes host checks but is unreadable when
# mounted by Docker, so reject it before starting any container.
if test -n "$(find "$SOURCE_ROOT" -type d ! -perm -o+x -print -quit)"; then
  echo "source root contains a directory not traversable by the container UID" >&2
  exit 1
fi
if test -n "$(find "$SOURCE_ROOT" -type f ! -perm -o+r -print -quit)"; then
  echo "source root contains a file not readable by the container UID" >&2
  exit 1
fi

python3 "$ROOT/tests/python/test_spec167_itiger_artifact_runner.py"
python3 "$ROOT/tests/python/test_spec167_itiger_job_contract.py"

docker image inspect "$IMAGE" >/dev/null
docker run --rm --network none --read-only \
  --tmpfs /tmp:rw,noexec,nosuid,size=128m \
  --entrypoint /opt/venv/bin/python \
  -e PYTHONPATH=/source/Experiments:/source/pythonWrapper:/source/NDNSF-DistributedRepo/pythonWrapper:/opt/ndnsf-app/python \
  -v "$SOURCE_ROOT:/source:ro" \
  "$IMAGE" -c \
  'import ndnsf; import NDNSF_DistributedRepo_Artifact_Itiger as r; assert len(r.build_schedule()) == 60'

# The remote source bundle intentionally excludes local compiled Python
# extensions. Prove that the sealed candidate's installed ndnsf/py_repoclient
# runtime is sufficient when only the checksum-bound experiment sources mount.
docker run --rm --network none --read-only \
  --tmpfs /tmp:rw,noexec,nosuid,size=64m \
  --entrypoint /opt/venv/bin/python \
  -e PYTHONPATH=/source/Experiments:/opt/ndnsf-app/python \
  -v "$ROOT/Experiments:/source/Experiments:ro" \
  "$IMAGE" -c \
  'import ndnsf, py_repoclient; import spec164_artifact_campaign; import NDNSF_DistributedRepo_Artifact_Itiger as r; assert hasattr(ndnsf, "FileSegmentedObjectProducer"); assert hasattr(spec164_artifact_campaign, "self_resource_totals"); assert len(r.build_schedule()) == 60'

tmp_dir=$(mktemp -d /tmp/spec167-nfd.XXXXXX)
cleanup() { rm -rf -- "$tmp_dir"; }
trap cleanup EXIT INT TERM
sed -e 's|@@NFD_SOCKET@@|/tmp/run/nfd.sock|g' \
    -e 's|@@TCP_PORT@@|26367|g' \
    -e 's|@@NODE_RANK@@|local|g' \
    "$JOBS/nfd.conf.in" > "$tmp_dir/nfd.conf"
docker run --rm --network none --read-only \
  --tmpfs /tmp:rw,nosuid,size=64m \
  -v "$tmp_dir/nfd.conf:/input/nfd.conf:ro" \
  --entrypoint /bin/bash "$IMAGE" -lc \
  'mkdir -p /tmp/run; timeout 2s nfd --config /input/nfd.conf >/tmp/nfd.log 2>&1; rc=$?; test "$rc" -eq 124'

"$JOBS/run-local-two-container-preflight.sh"
for subject in raw-segmented-ndn legacy-exact-packet digest-only signed-manifest; do
  SPEC167_LOCAL_SUBJECT="$subject" \
    "$JOBS/run-local-two-container-formal-smoke.sh"
done

echo SPEC167_LOCAL_CANDIDATE_PASS
