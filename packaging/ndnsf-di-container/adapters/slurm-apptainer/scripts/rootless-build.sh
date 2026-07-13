#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "usage: $0 --mode diagnostic|full --source-root PATH --project-root PATH --release-id ID --evidence-dir PATH --probe-base REF@sha256:HEX --gpu-build-base REF@sha256:HEX --gpu-runtime-base REF@sha256:HEX" >&2
  exit 2
}

mode= source_root= project_root= release_id= evidence_dir=
probe_base= gpu_build_base= gpu_runtime_base=
while (($#)); do
  case "$1" in
    --mode) mode=$2; shift 2 ;;
    --source-root) source_root=$2; shift 2 ;;
    --project-root) project_root=$2; shift 2 ;;
    --release-id) release_id=$2; shift 2 ;;
    --evidence-dir) evidence_dir=$2; shift 2 ;;
    --probe-base) probe_base=$2; shift 2 ;;
    --gpu-build-base) gpu_build_base=$2; shift 2 ;;
    --gpu-runtime-base) gpu_runtime_base=$2; shift 2 ;;
    *) usage ;;
  esac
done

[[ "$mode" == diagnostic || "$mode" == full ]] || usage
[[ "$release_id" =~ ^[A-Za-z0-9._+-]+$ ]] || { echo ROOTLESS_BUILD_RELEASE_ID_INVALID >&2; exit 4; }
digest_ref='^[^[:space:]]+@sha256:[a-f0-9]{64}$'
for ref in "$probe_base" "$gpu_build_base" "$gpu_runtime_base"; do
  [[ "$ref" =~ $digest_ref ]] || { echo ROOTLESS_BUILD_BASE_NOT_PINNED >&2; exit 4; }
done
[[ "${SLURM_JOB_ID:-}" =~ ^[0-9]+$ ]] || { echo ROOTLESS_BUILD_REQUIRES_SLURM >&2; exit 3; }
[[ -d "$source_root" ]] || { echo ROOTLESS_BUILD_SOURCE_MISSING >&2; exit 4; }

canonical() { realpath -m -- "$1"; }
project_root=$(canonical "$project_root")
evidence_dir=$(canonical "$evidence_dir")
source_root=$(canonical "$source_root")
if [[ "${NDNSF_SPEC110_ALLOW_TEST_ROOT:-0}" != 1 ]]; then
  [[ "$project_root" == "/project/${USER}/ndnsf-di" ]] || { echo ROOTLESS_BUILD_PROJECT_ROOT_INVALID >&2; exit 4; }
fi
[[ "$evidence_dir" == "$project_root/campaigns/spec110/"* ]] || { echo ROOTLESS_BUILD_EVIDENCE_ROOT_INVALID >&2; exit 4; }

scratch_base=
scratch_source=
if [[ -n "${SLURM_TMPDIR:-}" && -d "$SLURM_TMPDIR" && -w "$SLURM_TMPDIR" ]]; then
  scratch_base=$(canonical "$SLURM_TMPDIR")
  scratch_source=SLURM_TMPDIR
elif [[ -d /scratch && -w /scratch ]]; then
  scratch_base="/scratch/${USER}"
  scratch_source=TmpFS
else
  scratch_base="/tmp/${USER}"
  scratch_source=tmp-fallback
fi
scratch=$(canonical "$scratch_base/ndnsf-di/${SLURM_JOB_ID}/${release_id}")
case "$scratch" in
  /home/*|/project/*) echo ROOTLESS_BUILD_SCRATCH_DURABLE_PATH_FORBIDDEN >&2; exit 4 ;;
esac
if [[ "$scratch_source" != SLURM_TMPDIR && "$scratch" != *"/${USER}/"* ]]; then
  echo ROOTLESS_BUILD_SCRATCH_USER_INVALID >&2; exit 4
fi
[[ "$scratch" == *"/${SLURM_JOB_ID}/"* ]] || { echo ROOTLESS_BUILD_SCRATCH_JOB_INVALID >&2; exit 4; }

mkdir -p "$evidence_dir"
manifest="$evidence_dir/manifest.json"
[[ ! -e "$manifest" ]] || { echo ROOTLESS_BUILD_IDENTITY_ALREADY_EXECUTED >&2; exit 4; }
mkdir -p "$scratch"/{context,container/graphroot,container/runroot,container/cache,container/tmp,container/home,xdg}
chmod 0700 "$scratch" "$scratch/container"/* "$scratch/xdg"
graphroot="$scratch/container/graphroot"
runroot="$scratch/container/runroot"
cache="$scratch/container/cache"
tmp="$scratch/container/tmp"
export HOME="$scratch/container/home"
export XDG_RUNTIME_DIR="$scratch/xdg"
export XDG_CACHE_HOME="$cache"
export TMPDIR="$tmp"
export BUILDAH_ISOLATION=chroot

status=FAIL
reason=ROOTLESS_BUILD_INTERRUPTED
oci_digest=
oci_sha256=
sif_sha256=
release_path=
podman_version=
buildah_version=
apptainer_version=
source_seal_digest=
partial=
start_epoch=$(date +%s)

write_manifest() {
  local exit_code=$1
  STATUS="$status" REASON="$reason" EXIT_CODE="$exit_code" MODE="$mode" \
  RELEASE_ID="$release_id" PROJECT_ROOT="$project_root" SOURCE_ROOT="$source_root" \
  EVIDENCE_DIR="$evidence_dir" SCRATCH="$scratch" SCRATCH_SOURCE="$scratch_source" GRAPHROOT="$graphroot" RUNROOT="$runroot" \
  CACHE_ROOT="$cache" TMP_ROOT="$tmp" OCI_DIGEST="$oci_digest" OCI_SHA256="$oci_sha256" \
  SIF_SHA256="$sif_sha256" RELEASE_PATH="$release_path" PODMAN_VERSION="$podman_version" \
  BUILDAH_VERSION="$buildah_version" APPTAINER_VERSION="$apptainer_version" \
  SOURCE_SEAL_DIGEST="$source_seal_digest" START_EPOCH="$start_epoch" \
  python3 - "$manifest" <<'PY'
import datetime as dt, hashlib, json, os, sys
body = {
  'schemaVersion': 'spec110-rootless-build-evidence-v1',
  'status': os.environ['STATUS'], 'reasonCode': os.environ['REASON'],
  'exitCode': int(os.environ['EXIT_CODE']), 'diagnosticOnly': os.environ['MODE']=='diagnostic',
  'mode': os.environ['MODE'], 'releaseId': os.environ['RELEASE_ID'],
  'slurm': {'jobId': os.environ['SLURM_JOB_ID'], 'node': os.environ.get('SLURMD_NODENAME') or os.environ.get('HOSTNAME')},
  'source': {'root': os.environ['SOURCE_ROOT'], 'sealDigest': os.environ['SOURCE_SEAL_DIGEST'] or None},
  'storage': {'projectRoot': os.environ['PROJECT_ROOT'], 'selectedScratch': os.environ['SCRATCH'],
              'scratchSource': os.environ['SCRATCH_SOURCE'],
              'graphroot': os.environ['GRAPHROOT'], 'runroot': os.environ['RUNROOT'],
              'cache': os.environ['CACHE_ROOT'], 'tmp': os.environ['TMP_ROOT'],
              'releasePath': os.environ['RELEASE_PATH'] or None},
  'versions': {'podman': os.environ['PODMAN_VERSION'], 'buildah': os.environ['BUILDAH_VERSION'],
               'apptainer': os.environ['APPTAINER_VERSION']},
  'artifacts': {'ociDigest': os.environ['OCI_DIGEST'] or None,
                'ociArchiveSha256': os.environ['OCI_SHA256'] or None,
                'sifSha256': os.environ['SIF_SHA256'] or None},
  'startedAtEpoch': int(os.environ['START_EPOCH']),
  'finishedAt': dt.datetime.now(dt.timezone.utc).isoformat(),
  'scratchRetained': False,
  'physicalProduction': 'DEFERRED',
}
body['recordDigest']='sha256:'+hashlib.sha256(json.dumps(body,sort_keys=True,separators=(',',':')).encode()).hexdigest()
descriptor=os.open(sys.argv[1],os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o640)
with os.fdopen(descriptor, 'w', encoding='utf-8') as stream:
 json.dump(body, stream, indent=2, sort_keys=True); stream.write('\n'); stream.flush(); os.fsync(stream.fileno())
PY
}

finish() {
  code=$?
  trap - EXIT INT TERM
  if [[ "$status" != PASS && -n "$partial" && "$partial" == "$project_root/"*".partial.${SLURM_JOB_ID}" ]]; then
    rm -rf -- "$partial"
  fi
  rm -rf -- "$scratch"
  if ! write_manifest "$code"; then
    echo ROOTLESS_BUILD_EVIDENCE_WRITE_FAILED >&2
    [[ "$code" -ne 0 ]] || code=6
  fi
  exit "$code"
}
trap finish EXIT
trap 'reason=ROOTLESS_BUILD_SIGNAL_INT; exit 130' INT
trap 'reason=ROOTLESS_BUILD_SIGNAL_TERM; exit 143' TERM

for command in podman buildah apptainer python3 sha256sum; do
  command -v "$command" >/dev/null || { reason="ROOTLESS_BUILD_TOOL_MISSING:${command}"; exit 4; }
done
podman_version=$(podman --version | head -1)
buildah_version=$(buildah --version | head -1)
apptainer_version=$(apptainer version | head -1)

probe_file="$scratch/fsync-probe.bin"
dd if=/dev/zero of="$probe_file" bs=1M count=8 status=none
python3 - "$probe_file" <<'PY'
import os,sys
with open(sys.argv[1],'r+b') as stream: os.fsync(stream.fileno())
PY
rm -f "$probe_file"

context="$scratch/context"
if [[ "$mode" == diagnostic ]]; then
  printf '%s\n' rootless-build-probe >"$context/marker"
  cat >"$context/Dockerfile" <<EOF
FROM ${probe_base}
COPY marker /spec110-rootless-build-probe
RUN test "\$(cat /spec110-rootless-build-probe)" = rootless-build-probe
CMD ["/bin/sh", "-c", "test -f /spec110-rootless-build-probe"]
EOF
  dockerfile="$context/Dockerfile"
  build_args=()
else
  workspace="$source_root/workspace"
  lock="$workspace/packaging/ndnsf-di-container/oci/locks/gpu.lock"
  seal_tool="$workspace/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/seal-rootless-source.py"
  source_seal_digest=$(python3 "$seal_tool" verify --source-root "$source_root" --lock "$lock")
  git -C "$workspace" archive --format=tar HEAD | tar -xf - -C "$context"
  mkdir -p "$context/.spec110-build/sources"
  for dependency in "$source_root"/dependencies/*; do
    dependency_name=$(basename "$dependency")
    mkdir -p "$context/.spec110-build/sources/$dependency_name"
    git -C "$dependency" archive --format=tar HEAD | \
      tar -xf - -C "$context/.spec110-build/sources/$dependency_name"
  done
  cp "$source_root/source-seal.json" "$context/.spec110-build/source-seal.json"
  dockerfile="$context/packaging/ndnsf-di-container/oci/Dockerfile.gpu"
  build_args=(
    --build-arg "GPU_BUILD_BASE_IMAGE=$gpu_build_base"
    --build-arg "GPU_RUNTIME_BASE_IMAGE=$gpu_runtime_base"
    --build-arg "SOURCE_REVISION=$(git -C "$workspace" rev-parse HEAD)"
    --build-arg "RELEASE_ID=$release_id"
    --build-arg DEPENDENCY_SOURCE_MODE=sealed
  )
  python3 "$workspace/packaging/ndnsf-di-container/oci/scripts/scan-secrets.py" \
    --path "$context" --scope source --output "$evidence_dir/source-secret-scan.json"
fi

image="localhost/spec110/${release_id}:sealed"
build_log="$evidence_dir/build.log"
podman --root "$graphroot" --runroot "$runroot" build --format oci --pull=always \
  --tag "$image" -f "$dockerfile" "${build_args[@]}" "$context" >"$build_log" 2>&1 || {
    reason=ROOTLESS_BUILD_OCI_FAILED
    exit 5
  }

archive="$scratch/runtime.oci.tar"
podman --root "$graphroot" --runroot "$runroot" save --format oci-archive -o "$archive" "$image" || {
  reason=ROOTLESS_BUILD_OCI_EXPORT_FAILED
  exit 5
}
inspector="$(dirname "$0")/inspect-oci-archive.py"
oci_digest=$(python3 "$inspector" "$archive") || { reason=ROOTLESS_BUILD_OCI_INVALID; exit 5; }
oci_sha256="sha256:$(sha256sum "$archive" | cut -d' ' -f1)"

if [[ "$mode" == diagnostic ]]; then
  target="$evidence_dir/artifacts"
else
  target="$project_root/releases/$release_id"
fi
[[ ! -e "$target" ]] || { reason=ROOTLESS_BUILD_RELEASE_EXISTS; exit 4; }
partial="$target.partial.${SLURM_JOB_ID}"
mkdir -p "$partial"
cp "$archive" "$partial/runtime.oci.tar.partial"
python3 - "$partial/runtime.oci.tar.partial" <<'PY'
import os,sys
with open(sys.argv[1],'r+b') as stream: os.fsync(stream.fileno())
PY
mv "$partial/runtime.oci.tar.partial" "$partial/runtime.oci.tar"

apptainer build "$partial/runtime.sif.partial" "oci-archive:$partial/runtime.oci.tar" || {
  reason=ROOTLESS_BUILD_SIF_FAILED
  exit 5
}
[[ -s "$partial/runtime.sif.partial" ]] || { reason=ROOTLESS_BUILD_SIF_EMPTY; exit 5; }
if [[ "$mode" == diagnostic ]]; then
  apptainer exec --containall --no-home "$partial/runtime.sif.partial" /bin/sh -c \
    'test -f /spec110-rootless-build-probe' || { reason=ROOTLESS_BUILD_SIF_EXEC_FAILED; exit 5; }
else
  apptainer exec --containall --no-home "$partial/runtime.sif.partial" \
    /usr/local/bin/ndnsf-di-probe-runtime --mode static || { reason=ROOTLESS_BUILD_SIF_EXEC_FAILED; exit 5; }
fi
mv "$partial/runtime.sif.partial" "$partial/runtime.sif"
sif_sha256="sha256:$(sha256sum "$partial/runtime.sif" | cut -d' ' -f1)"
printf '%s  %s\n' "${oci_sha256#sha256:}" runtime.oci.tar >"$partial/SHA256SUMS"
printf '%s  %s\n' "${sif_sha256#sha256:}" runtime.sif >>"$partial/SHA256SUMS"
(cd "$partial" && sha256sum -c SHA256SUMS)
mv "$partial" "$target"
release_path="$target"
status=PASS
reason=ROOTLESS_BUILD_PASS
exit 0
