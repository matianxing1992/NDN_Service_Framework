#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "usage: $0 --mode diagnostic|full --source-root PATH --project-root PATH --release-id ID --evidence-dir PATH --builder-mode auto|host|apptainer-sif --builder-oci REF@sha256:HEX --probe-base REF@sha256:HEX --foundation-base REF@sha256:HEX --gpu-build-base REF@sha256:HEX --gpu-runtime-base REF@sha256:HEX" >&2
  exit 2
}

mode='' source_root='' project_root='' release_id='' evidence_dir=''
probe_base='' foundation_base='' gpu_build_base='' gpu_runtime_base=''
builder_mode=auto
builder_oci=quay.io/buildah/stable@sha256:8570703f0feb3f39d180e932a2ec8e350ee860790062a5ecd5a3b3ac51f337c5
while (($#)); do
  case "$1" in
    --mode) mode=$2; shift 2 ;;
    --source-root) source_root=$2; shift 2 ;;
    --project-root) project_root=$2; shift 2 ;;
    --release-id) release_id=$2; shift 2 ;;
    --evidence-dir) evidence_dir=$2; shift 2 ;;
    --builder-mode) builder_mode=$2; shift 2 ;;
    --builder-oci) builder_oci=$2; shift 2 ;;
    --probe-base) probe_base=$2; shift 2 ;;
    --foundation-base) foundation_base=$2; shift 2 ;;
    --gpu-build-base) gpu_build_base=$2; shift 2 ;;
    --gpu-runtime-base) gpu_runtime_base=$2; shift 2 ;;
    *) usage ;;
  esac
done

[[ "$mode" == diagnostic || "$mode" == full ]] || usage
[[ "$builder_mode" == auto || "$builder_mode" == host || "$builder_mode" == apptainer-sif ]] || usage
[[ "$release_id" =~ ^[A-Za-z0-9._+-]+$ ]] || { echo ROOTLESS_BUILD_RELEASE_ID_INVALID >&2; exit 4; }
digest_ref='^[^[:space:]]+@sha256:[a-f0-9]{64}$'
for ref in "$probe_base" "$gpu_build_base" "$gpu_runtime_base" "$builder_oci"; do
  [[ "$ref" =~ $digest_ref ]] || { echo ROOTLESS_BUILD_BASE_NOT_PINNED >&2; exit 4; }
done
if [[ "$mode" == full ]]; then
  [[ "$foundation_base" =~ $digest_ref ]] || { echo ROOTLESS_BUILD_FOUNDATION_BASE_NOT_PINNED >&2; exit 4; }
fi
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
  scratch_source='tmp-fallback'
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
foundation_local_image=
effective_builder_mode=
builder_oci_digest="sha256:${builder_oci##*@sha256:}"
builder_sif_sha256=
builder_namespace_mode=
partial=
cleanup_error=
start_epoch=$(date +%s)

write_manifest() {
  local exit_code=$1
  STATUS="$status" REASON="$reason" EXIT_CODE="$exit_code" MODE="$mode" \
  RELEASE_ID="$release_id" PROJECT_ROOT="$project_root" SOURCE_ROOT="$source_root" \
  EVIDENCE_DIR="$evidence_dir" SCRATCH="$scratch" SCRATCH_SOURCE="$scratch_source" GRAPHROOT="$graphroot" RUNROOT="$runroot" \
  CACHE_ROOT="$cache" TMP_ROOT="$tmp" OCI_DIGEST="$oci_digest" OCI_SHA256="$oci_sha256" \
  SIF_SHA256="$sif_sha256" RELEASE_PATH="$release_path" PODMAN_VERSION="$podman_version" \
  BUILDAH_VERSION="$buildah_version" APPTAINER_VERSION="$apptainer_version" \
  REQUESTED_BUILDER_MODE="$builder_mode" EFFECTIVE_BUILDER_MODE="$effective_builder_mode" \
  BUILDER_OCI="$builder_oci" BUILDER_OCI_DIGEST="$builder_oci_digest" \
  BUILDER_SIF_SHA256="$builder_sif_sha256" BUILDER_NAMESPACE_MODE="$builder_namespace_mode" \
  FOUNDATION_BASE="$foundation_base" FOUNDATION_LOCAL_IMAGE="$foundation_local_image" CLEANUP_ERROR="$cleanup_error" SOURCE_SEAL_DIGEST="$source_seal_digest" START_EPOCH="$start_epoch" \
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
  'builder': {'requestedMode': os.environ['REQUESTED_BUILDER_MODE'],
              'effectiveMode': os.environ['EFFECTIVE_BUILDER_MODE'] or None,
              'foundationBaseImage': os.environ['FOUNDATION_BASE'] or None,
              'foundationLocalImage': os.environ['FOUNDATION_LOCAL_IMAGE'] or None,
              'ociRef': os.environ['BUILDER_OCI'],
              'ociDigest': os.environ['BUILDER_OCI_DIGEST'],
              'sifSha256': os.environ['BUILDER_SIF_SHA256'] or None,
              'namespaceMode': os.environ['BUILDER_NAMESPACE_MODE'] or None,
              'storageDriver': 'vfs' if os.environ['EFFECTIVE_BUILDER_MODE']=='apptainer-sif' else None,
              'isolation': 'chroot' if os.environ['EFFECTIVE_BUILDER_MODE']=='apptainer-sif' else None},
  'artifacts': {'ociDigest': os.environ['OCI_DIGEST'] or None,
                'ociArchiveSha256': os.environ['OCI_SHA256'] or None,
                'sifSha256': os.environ['SIF_SHA256'] or None},
  'startedAtEpoch': int(os.environ['START_EPOCH']),
  'finishedAt': dt.datetime.now(dt.timezone.utc).isoformat(),
  'cleanupError': os.environ['CLEANUP_ERROR'] or None,
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
    if ! rm -rf -- "$partial"; then
      cleanup_error=ROOTLESS_BUILD_PARTIAL_CLEANUP_FAILED
    fi
  fi
  if ! rm -rf -- "$scratch"; then
    # Fakeroot may report EPERM for individual root-owned entries even when
    # the scratch tree has been removed completely.  Only retain a cleanup
    # failure when the durable scratch path still exists.
    if [[ -e "$scratch" ]]; then
      cleanup_error=${cleanup_error:-ROOTLESS_BUILD_SCRATCH_CLEANUP_FAILED}
      if [[ "$status" == PASS && "$code" -eq 0 ]]; then
        status=FAIL
        reason=$cleanup_error
        code=6
      fi
    fi
  fi
  if ! write_manifest "$code"; then
    echo ROOTLESS_BUILD_EVIDENCE_WRITE_FAILED >&2
    if [[ "$status" == PASS && -n "$release_path" ]]; then
      case "$release_path" in
        "$evidence_dir/artifacts"|"$project_root/releases/$release_id") rm -rf -- "$release_path" ;;
        *) echo ROOTLESS_BUILD_EVIDENCE_ROLLBACK_PATH_INVALID >&2 ;;
      esac
    fi
    [[ "$code" -ne 0 ]] || code=6
  fi
  exit "$code"
}
trap finish EXIT
trap 'reason=ROOTLESS_BUILD_SIGNAL_INT; exit 130' INT
trap 'reason=ROOTLESS_BUILD_SIGNAL_TERM; exit 143' TERM

for command in apptainer python3 sha256sum; do
  command -v "$command" >/dev/null || { reason="ROOTLESS_BUILD_TOOL_MISSING:${command}"; exit 4; }
done
apptainer_version=$(apptainer version | head -1)

case "$builder_mode" in
  host) effective_builder_mode=host ;;
  apptainer-sif) effective_builder_mode=apptainer-sif ;;
  auto)
    if command -v podman >/dev/null && command -v buildah >/dev/null; then
      effective_builder_mode=host
    else
      effective_builder_mode=apptainer-sif
    fi
    ;;
esac
if [[ "$effective_builder_mode" == host ]]; then
  for command in podman buildah; do
    command -v "$command" >/dev/null || { reason="ROOTLESS_BUILD_TOOL_MISSING:${command}"; exit 4; }
  done
  podman_version=$(podman --version | head -1)
  buildah_version=$(buildah --version | head -1)
fi

probe_file="$scratch/fsync-probe.bin"
dd if=/dev/zero of="$probe_file" bs=1M count=8 status=none
python3 - "$probe_file" <<'PY'
import os,sys
with open(sys.argv[1],'r+b') as stream: os.fsync(stream.fileno())
PY
rm -f "$probe_file"

builder_sif="$scratch/container/builder.sif"
builder_exec() {
  apptainer exec --containall --no-home --cleanenv --fakeroot \
    --bind "$scratch:$scratch" "$builder_sif" \
    env HOME="$HOME" XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" XDG_CACHE_HOME="$XDG_CACHE_HOME" \
        TMPDIR="$TMPDIR" BUILDAH_ISOLATION=chroot "$@"
}
if [[ "$effective_builder_mode" == apptainer-sif ]]; then
  export APPTAINER_CACHEDIR="$cache/apptainer"
  export APPTAINER_TMPDIR="$tmp"
  mkdir -p "$APPTAINER_CACHEDIR"
  apptainer build "$builder_sif.partial" "docker://$builder_oci" \
    >"$evidence_dir/builder-sif.log" 2>&1 || {
      reason=ROOTLESS_BUILD_BUILDER_SIF_FAILED
      exit 5
    }
  [[ -s "$builder_sif.partial" ]] || { reason=ROOTLESS_BUILD_BUILDER_SIF_EMPTY; exit 5; }
  mv "$builder_sif.partial" "$builder_sif"
  builder_sif_sha256="sha256:$(sha256sum "$builder_sif" | cut -d' ' -f1)"
  builder_namespace_mode=apptainer-fakeroot
  buildah_version=$(builder_exec buildah --version | head -1) || {
    reason=ROOTLESS_BUILD_BUILDER_EXEC_FAILED
    exit 5
  }
  builder_exec buildah --root "$graphroot" --runroot "$runroot" \
    --storage-driver vfs info >"$evidence_dir/builder-probe.log" 2>&1 || {
      reason=ROOTLESS_BUILD_USER_NAMESPACE_UNAVAILABLE
      exit 5
    }
fi

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
  lock="$context/packaging/ndnsf-di-container/oci/locks/gpu.lock"
  workspace_archive="$source_root/workspace.tar"
  sealed_root="$source_root/.spec110-build"
  read -r source_seal_digest source_revision < <(python3 - "$source_root/source-seal.json" "$workspace_archive" "$sealed_root" <<'PY'
import hashlib
import json
import pathlib
import sys

seal_path, workspace_archive, sealed_root = map(pathlib.Path, sys.argv[1:])
value = json.loads(seal_path.read_text(encoding="utf-8"))
digest = value.pop("sealDigest", None)
encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
if value.get("schemaVersion") != "spec110-oci-source-seal-v1" or digest != "sha256:" + hashlib.sha256(encoded).hexdigest():
    raise SystemExit("ROOTLESS_BUILD_SOURCE_SEAL_INVALID")
workspace = value.get("workspace", {})
archive = workspace_archive
if not archive.is_file():
    raise SystemExit("ROOTLESS_BUILD_WORKSPACE_ARCHIVE_MISSING")
if archive.stat().st_size != workspace.get("archiveBytes"):
    raise SystemExit("ROOTLESS_BUILD_WORKSPACE_ARCHIVE_SIZE_MISMATCH")
if "sha256:" + hashlib.sha256(archive.read_bytes()).hexdigest() != workspace.get("archiveDigest"):
    raise SystemExit("ROOTLESS_BUILD_WORKSPACE_ARCHIVE_DIGEST_MISMATCH")
for name, row in value.get("dependencies", {}).items():
    archive = sealed_root / row["archivePath"]
    if not archive.is_file():
        raise SystemExit(f"ROOTLESS_BUILD_DEPENDENCY_ARCHIVE_MISSING:{name}")
    if archive.stat().st_size != row.get("archiveBytes"):
        raise SystemExit(f"ROOTLESS_BUILD_DEPENDENCY_ARCHIVE_SIZE_MISMATCH:{name}")
    if "sha256:" + hashlib.sha256(archive.read_bytes()).hexdigest() != row.get("archiveDigest"):
        raise SystemExit(f"ROOTLESS_BUILD_DEPENDENCY_ARCHIVE_DIGEST_MISMATCH:{name}")
print(digest, workspace.get("revision"))
PY
  )
  tar -xf "$workspace_archive" -C "$context"
  cp -a "$sealed_root" "$context/.spec110-build"
  foundation_dockerfile="$context/packaging/ndnsf-di-container/oci/Dockerfile.foundation"
  foundation_local_image="localhost/spec110/${release_id}:foundation"
  foundation_build_log="$evidence_dir/foundation-build.log"
  foundation_build_args=(
    --build-arg "FOUNDATION_BASE_IMAGE=$foundation_base"
    --build-arg "SOURCE_REVISION=$source_revision"
    --build-arg "DEPENDENCY_SOURCE_MODE=sealed"
  )
  if [[ "$effective_builder_mode" == host ]]; then
    podman --root "$graphroot" --runroot "$runroot" build --format oci --pull=missing \
      --target foundation --tag "$foundation_local_image" -f "$foundation_dockerfile" \
      "${foundation_build_args[@]}" "$context" >"$foundation_build_log" 2>&1 || {
        reason=ROOTLESS_BUILD_FOUNDATION_FAILED
        exit 5
      }
  else
    builder_exec buildah --root "$graphroot" --runroot "$runroot" --storage-driver vfs \
      bud --isolation chroot --format oci --pull=missing --target foundation \
      --tag "$foundation_local_image" -f "$foundation_dockerfile" \
      "${foundation_build_args[@]}" "$context" >"$foundation_build_log" 2>&1 || {
        reason=ROOTLESS_BUILD_FOUNDATION_FAILED
        exit 5
      }
  fi
  foundation_image="$foundation_local_image"
  dockerfile="$context/packaging/ndnsf-di-container/oci/Dockerfile.gpu"
  onnx_url=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["onnxRuntimeCpp"]["url"])' "$lock")
  onnx_sha256=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["onnxRuntimeCpp"]["sha256"])' "$lock")
  python_base=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["baseImages"]["python"])' "$lock")
  build_args=(
    --build-arg "FOUNDATION_IMAGE=$foundation_image"
    --build-arg "PYTHON_BASE_IMAGE=$python_base"
    --build-arg "GPU_BUILD_BASE_IMAGE=$gpu_build_base"
    --build-arg "GPU_RUNTIME_BASE_IMAGE=$gpu_runtime_base"
    --build-arg "SOURCE_REVISION=$source_revision"
    --build-arg "FOUNDATION_SOURCE_REVISION=$source_revision"
    --build-arg "RELEASE_ID=$release_id"
    --build-arg "ONNXRUNTIME_CPP_URL=$onnx_url"
    --build-arg "ONNXRUNTIME_CPP_SHA256=$onnx_sha256"
  )
  python3 "$context/packaging/ndnsf-di-container/oci/scripts/scan-secrets.py" \
    --path "$context/wscript" \
    --path "$context/examples" \
    --path "$context/ndn-service-framework" \
    --path "$context/NDNSF-DistributedRepo" \
    --path "$context/NDNSF-DistributedInference" \
    --path "$context/packaging/ndnsf-di-container" \
    --scope source --output "$evidence_dir/source-secret-scan.json"
fi

image="localhost/spec110/${release_id}:sealed"
build_log="$evidence_dir/build.log"
archive="$scratch/runtime.oci.tar"
if [[ "$effective_builder_mode" == host ]]; then
  podman --root "$graphroot" --runroot "$runroot" build --format oci --pull=missing \
    --tag "$image" -f "$dockerfile" "${build_args[@]}" "$context" >"$build_log" 2>&1 || {
      reason=ROOTLESS_BUILD_OCI_FAILED
      exit 5
    }
  podman --root "$graphroot" --runroot "$runroot" save --format oci-archive -o "$archive" "$image" || {
    reason=ROOTLESS_BUILD_OCI_EXPORT_FAILED
    exit 5
  }
else
  builder_exec buildah --root "$graphroot" --runroot "$runroot" --storage-driver vfs \
    bud --isolation chroot --format oci --pull=missing --tag "$image" \
    -f "$dockerfile" "${build_args[@]}" "$context" >"$build_log" 2>&1 || {
      reason=ROOTLESS_BUILD_OCI_FAILED
      exit 5
    }
  builder_exec buildah --root "$graphroot" --runroot "$runroot" --storage-driver vfs \
    push "$image" "oci-archive:$archive" || {
      reason=ROOTLESS_BUILD_OCI_EXPORT_FAILED
      exit 5
    }
fi
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
# Compute nodes may not have a real /home/$USER, while the SIF root is
# read-only.  Keep the acceptance probe independent of the submitter's login
# working directory and home so a valid SIF is not rejected after materialization.
probe_sif() {
  apptainer exec --containall --no-home --pwd / --env HOME=/tmp \
    "$partial/runtime.sif.partial" "$@"
}
if [[ "$mode" == diagnostic ]]; then
  probe_sif /bin/sh -c \
    'test -f /spec110-rootless-build-probe' || { reason=ROOTLESS_BUILD_SIF_EXEC_FAILED; exit 5; }
else
  probe_sif \
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
