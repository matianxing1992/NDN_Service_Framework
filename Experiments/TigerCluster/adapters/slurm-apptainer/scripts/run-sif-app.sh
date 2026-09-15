#!/usr/bin/env bash
# Run a verified base SIF with an immutable external NDNSF-DI APP.
set -euo pipefail
umask 077

usage() {
  cat >&2 <<'EOF'
usage: run-sif-app.sh --base-sif PATH --app PATH --app-record PATH \
  --project PATH --scratch PATH --identity PATH [--release-bind PATH] \
  [--models PATH] [--artifacts PATH] [--evidence PATH] [--gpu-count N] \
  [--apptainer PATH] [--expected-apptainer VERSION] [--local] -- COMMAND [ARGS...]

The default mode requires SLURM_JOB_ID.  --local is an explicit pre-Tiger
verification mode and keeps the same cleanenv/containall/mount contract.
EOF
  exit 2
}

base_sif=''; app=''; app_record=''; project=''; scratch=''; identity=''
release=''; models=''; artifacts=''; evidence=''; gpu_count=''
apptainer_bin=/usr/local/bin/apptainer; expected_version=1.5.3; local_mode=0
while (($#)); do
  case "$1" in
    --base-sif) base_sif="$2"; shift 2 ;;
    --app) app="$2"; shift 2 ;;
    --app-record) app_record="$2"; shift 2 ;;
    --project) project="$2"; shift 2 ;;
    --scratch) scratch="$2"; shift 2 ;;
    --identity) identity="$2"; shift 2 ;;
    --release-bind) release="$2"; shift 2 ;;
    --models) models="$2"; shift 2 ;;
    --artifacts) artifacts="$2"; shift 2 ;;
    --evidence) evidence="$2"; shift 2 ;;
    --gpu-count) gpu_count="$2"; shift 2 ;;
    --apptainer) apptainer_bin="$2"; shift 2 ;;
    --expected-apptainer) expected_version="$2"; shift 2 ;;
    --local) local_mode=1; shift ;;
    --) shift; break ;;
    *) usage ;;
  esac
done
if [ -z "$base_sif" ] || [ -z "$app" ] || [ -z "$app_record" ] || \
  [ -z "$project" ] || [ -z "$scratch" ] || [ -z "$identity" ] || \
  [ "$#" -eq 0 ]; then
  usage
fi
[ "$local_mode" = 1 ] || [ -n "${SLURM_JOB_ID-}" ] || {
  echo APPTAINER_PAIR_RUN_REQUIRES_SLURM >&2; exit 3;
}
if [ ! -f "$apptainer_bin" ] || [ ! -x "$apptainer_bin" ]; then
  echo APPTAINER_PAIR_APPTAINER_MISSING >&2; exit 4;
fi
# Bundle identity inputs must be absolute and have no symlink in any component.
# Apptainer may be a launcher symlink; its resolved regular binary is pinned
# and digest-checked below.
python3 - "$base_sif" "$app" "$app_record" "$apptainer_bin" <<'PY'
import sys
from pathlib import Path
for index, raw in enumerate(sys.argv[1:]):
    path = Path(raw)
    if not path.is_absolute() or any(char in raw for char in ",:\n\r"):
        raise SystemExit("APPTAINER_PAIR_PATH_NOT_ABSOLUTE:" + raw)
    current = Path(path.anchor)
    for component in path.parts[1:]:
        current /= component
        if index < 3 and current.is_symlink():
            raise SystemExit("APPTAINER_PAIR_SYMLINK_INPUT_FORBIDDEN:" + str(current))
PY
# APP and its identity are content-addressed inputs.  Do not resolve a mutable
# symlink and then bind a different tree than the one that was reviewed.
for immutable_input in "$base_sif" "$app" "$app_record"; do
  [ ! -L "$immutable_input" ] || {
    echo APPTAINER_PAIR_SYMLINK_INPUT_FORBIDDEN:"$immutable_input" >&2; exit 4;
  }
done
release=${release:-$project/releases}
models=${models:-$project/models}
artifacts=${artifacts:-$project/artifacts}
evidence=${evidence:-$project/evidence}
# Every host path used as a bind source is checked before canonicalisation.
python3 - "$project" "$identity" "$release" "$models" "$artifacts" "$evidence" "$scratch" <<'PY'
import sys
from pathlib import Path
for raw in sys.argv[1:]:
    path = Path(raw)
    if not path.is_absolute() or any(char in raw for char in ",:\n\r"):
        raise SystemExit("APPTAINER_PAIR_BIND_PATH_INVALID:" + raw)
    current = Path(path.anchor)
    for component in path.parts[1:]:
        current /= component
        if current.is_symlink():
            raise SystemExit("APPTAINER_PAIR_BIND_SYMLINK_FORBIDDEN:" + str(current))
PY
apptainer_bin=$(readlink -m "$apptainer_bin")
base_sif=$(readlink -m "$base_sif")
app=$(readlink -m "$app")
app_record=$(readlink -m "$app_record")
project=$(readlink -m "$project")
identity=$(readlink -m "$identity")
release=$(readlink -m "$release"); models=$(readlink -m "$models")
artifacts=$(readlink -m "$artifacts"); evidence=$(readlink -m "$evidence")
[ -f "$base_sif" ] || { echo APPTAINER_PAIR_BASE_SIF_MISSING >&2; exit 4; }
if [ ! -f "$apptainer_bin" ] || [ ! -x "$apptainer_bin" ]; then
  echo APPTAINER_PAIR_APPTAINER_NOT_REGULAR >&2; exit 4;
fi
[ -d "$app" ] || { echo APPTAINER_PAIR_APP_MISSING >&2; exit 4; }
[ -f "$app_record" ] || { echo APPTAINER_PAIR_APP_RECORD_MISSING >&2; exit 4; }
[ -d "$identity" ] || { echo APPTAINER_PAIR_IDENTITY_MISSING >&2; exit 4; }
for path in "$release" "$models" "$artifacts" "$evidence"; do
  [ -d "$path" ] || { echo APPTAINER_PAIR_BIND_MISSING:"$path" >&2; exit 4; }
done

script_dir=$(CDPATH=; cd -- "$(dirname -- "$0")" && pwd)
validator="$script_dir/validate-sif-app.py"
[ -f "$validator" ] || { echo APPTAINER_PAIR_VALIDATOR_MISSING >&2; exit 4; }
validation_json=$(python3 "$validator" --manifest "$app_record" --app-root "$app" --base-sif "$base_sif")
app_identity=$(stat -Lc '%d:%i' "$app")
record_identity=$(stat -Lc '%d:%i' "$app_record")
base_identity=$(stat -Lc '%d:%i' "$base_sif")
app_lib_identity=$(stat -Lc '%d:%i' "$app/lib")
app_bin_identity=$(stat -Lc '%d:%i' "$app/bin")
app_python_identity=$(stat -Lc '%d:%i' "$app/python")
app_manifest_identity=$(stat -Lc '%d:%i' "$app/manifest")
app_replay_identity=$(stat -Lc '%d:%i' "$app/replay")
[ ! -w "$app_record" ] || { echo APPTAINER_PAIR_MANIFEST_MUTABLE >&2; exit 4; }
if find "$app" -type f -perm /022 -print -quit | grep -q .; then
  echo APPTAINER_PAIR_APP_MUTABLE >&2; exit 4
fi

normalize_version() { printf '%s\n' "$1" | sed -E 's/[^0-9]*([0-9]+\.[0-9]+\.[0-9]+).*/\1/'; }
local_version=$("$apptainer_bin" version)
[ "$(normalize_version "$local_version")" = "$(normalize_version "$expected_version")" ] || {
  echo "APPTAINER_PAIR_VERSION_MISMATCH local=$local_version expected=$expected_version" >&2
  exit 4
}
actual_apptainer_sha=$(sha256sum "$apptainer_bin" | awk '{print "sha256:"$1}')
recorded_apptainer_sha=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["apptainerSha256"])' <<<"$validation_json")
[ "$actual_apptainer_sha" = "$recorded_apptainer_sha" ] || {
  echo APPTAINER_PAIR_BINARY_DIGEST_MISMATCH >&2; exit 4;
}
if [ -z "$gpu_count" ]; then gpu_count=${SLURM_GPUS_ON_NODE:-0}; fi
case "$gpu_count" in ''|*[!0-9]*) echo APPTAINER_PAIR_GPU_COUNT_INVALID >&2; exit 2 ;; esac
job_id=${SLURM_JOB_ID:-local-$$}
case "$scratch" in /*) ;; *) echo APPTAINER_PAIR_SCRATCH_INVALID >&2; exit 4 ;; esac
case "$scratch" in /tmp/*) ;; *) echo APPTAINER_PAIR_SCRATCH_INVALID >&2; exit 4 ;; esac
case "$scratch" in *..*) echo APPTAINER_PAIR_SCRATCH_INVALID >&2; exit 4 ;; esac
scratch_resolved=$(readlink -f "$scratch")
[ "$scratch_resolved" = "$scratch" ] || { echo APPTAINER_PAIR_SCRATCH_SYMLINK_FORBIDDEN >&2; exit 4; }
scratch=$scratch_resolved
if [ "$local_mode" = 0 ]; then
  [ "$scratch" = "/tmp/ndnsf-di-$SLURM_JOB_ID" ] || {
    echo APPTAINER_PAIR_SCRATCH_NOT_JOB_SCOPED >&2; exit 4;
  }
else
  case "$scratch" in /tmp/ndnsf-di-local-*) ;; *)
    echo APPTAINER_PAIR_LOCAL_SCRATCH_NOT_SCOPED >&2; exit 4 ;;
  esac
fi
[ ! -e "$scratch" ] || { echo APPTAINER_PAIR_SCRATCH_EXISTS >&2; exit 4; }
scratch_parent=$(dirname -- "$scratch")
scratch_name=$(basename -- "$scratch")
exec {scratch_parent_fd}<"$scratch_parent" || {
  echo APPTAINER_PAIR_SCRATCH_PARENT_OPEN_FAILED >&2; exit 4;
}
scratch_created=0
scratch_fd=-1
scratch_identity=''

# Scratch is a per-run private directory.  Register cleanup before any
# post-mkdir probe so a failed probe cannot leak the newly-created directory.
# Cleanup uses a fixed parent descriptor, a fixed root descriptor, and saved
# inode identities before removing entries or the root path.
cleanup_scratch() {
  local status=$?
  local cleanup_status=0
  trap - EXIT
  set +e
  if [ "$scratch_created" -eq 1 ] && [ "$scratch_fd" -ge 0 ]; then
    python3 - "$scratch_parent_fd" "$scratch_name" "$scratch_fd" "$scratch_identity" <<'PY'
import os
import stat
import sys

parent_fd = int(sys.argv[1])
root_name = sys.argv[2]
root_fd = int(sys.argv[3])
identity_text = sys.argv[4]
flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)

def fail():
    raise SystemExit(1)

try:
    info = os.fstat(root_fd)
    expected = (info.st_dev, info.st_ino)
    if identity_text:
        recorded = tuple(int(value) for value in identity_text.split(":", 1))
        if recorded != expected:
            fail()
    if not stat.S_ISDIR(info.st_mode):
        fail()
    root_entry = os.stat(root_name, dir_fd=parent_fd, follow_symlinks=False)
    if (root_entry.st_dev, root_entry.st_ino) != expected:
        fail()
    os.fchmod(root_fd, info.st_mode | 0o700)

    def remove_children(directory_fd):
        with os.scandir(directory_fd) as entries:
            for entry in entries:
                saved = os.stat(entry.name, dir_fd=directory_fd, follow_symlinks=False)
                saved_identity = (saved.st_dev, saved.st_ino)
                if stat.S_ISLNK(saved.st_mode):
                    current = os.stat(entry.name, dir_fd=directory_fd, follow_symlinks=False)
                    if (current.st_dev, current.st_ino) != saved_identity:
                        fail()
                    os.unlink(entry.name, dir_fd=directory_fd)
                    continue
                if stat.S_ISDIR(saved.st_mode):
                    child_fd = os.open(entry.name, flags, dir_fd=directory_fd)
                    try:
                        child_info = os.fstat(child_fd)
                        if ((child_info.st_dev, child_info.st_ino) != saved_identity
                                or not stat.S_ISDIR(child_info.st_mode)):
                            fail()
                        os.fchmod(child_fd, child_info.st_mode | 0o700)
                        remove_children(child_fd)
                        current = os.stat(entry.name, dir_fd=directory_fd,
                                          follow_symlinks=False)
                        if ((current.st_dev, current.st_ino) != saved_identity
                                or not stat.S_ISDIR(current.st_mode)):
                            fail()
                        os.rmdir(entry.name, dir_fd=directory_fd)
                    finally:
                        os.close(child_fd)
                else:
                    current = os.stat(entry.name, dir_fd=directory_fd, follow_symlinks=False)
                    if (current.st_dev, current.st_ino) != saved_identity:
                        fail()
                    os.unlink(entry.name, dir_fd=directory_fd)

    remove_children(root_fd)
    current_root = os.stat(root_name, dir_fd=parent_fd, follow_symlinks=False)
    if ((current_root.st_dev, current_root.st_ino) != expected
            or not stat.S_ISDIR(current_root.st_mode)):
        fail()
    os.rmdir(root_name, dir_fd=parent_fd)
except (OSError, ValueError):
    raise SystemExit(1)
PY
    cleanup_status=$?
  else
    cleanup_status=1
  fi
  if [ "$cleanup_status" -ne 0 ]; then
    echo APPTAINER_PAIR_SCRATCH_CLEANUP_FAILED:"$scratch" >&2
    [ "$status" -ne 0 ] || status=4
  fi
  exit "$status"
}
trap cleanup_scratch EXIT

mkdir "$scratch" || { echo APPTAINER_PAIR_SCRATCH_BUSY >&2; exit 4; }
scratch_created=1
exec {scratch_fd}<"$scratch" || {
  echo APPTAINER_PAIR_SCRATCH_OPEN_FAILED >&2; exit 4;
}
scratch_identity=$(stat -Lc '%d:%i' "/proc/self/fd/$scratch_fd")
mkdir "$scratch/home"
chmod 700 "$scratch" "$scratch/home"
[ "$(stat -c '%u' "$scratch")" = "$(id -u)" ] || {
  echo APPTAINER_PAIR_SCRATCH_OWNER_MISMATCH >&2; exit 4;
}
revalidated_json=$(python3 "$validator" --manifest "$app_record" --app-root "$app" --base-sif "$base_sif")
[ "$app_identity" = "$(stat -Lc '%d:%i' "$app")" ] || {
  echo APPTAINER_PAIR_APP_IDENTITY_CHANGED >&2; exit 4;
}
[ "$record_identity" = "$(stat -Lc '%d:%i' "$app_record")" ] || {
  echo APPTAINER_PAIR_MANIFEST_IDENTITY_CHANGED >&2; exit 4;
}
[ "$base_identity" = "$(stat -Lc '%d:%i' "$base_sif")" ] || {
  echo APPTAINER_PAIR_BASE_IDENTITY_CHANGED >&2; exit 4;
}
[ "$app_lib_identity" = "$(stat -Lc '%d:%i' "$app/lib")" ] || {
  echo APPTAINER_PAIR_APP_LIB_IDENTITY_CHANGED >&2; exit 4;
}
[ "$app_bin_identity" = "$(stat -Lc '%d:%i' "$app/bin")" ] || {
  echo APPTAINER_PAIR_APP_BIN_IDENTITY_CHANGED >&2; exit 4;
}
[ "$app_python_identity" = "$(stat -Lc '%d:%i' "$app/python")" ] || {
  echo APPTAINER_PAIR_APP_PYTHON_IDENTITY_CHANGED >&2; exit 4;
}
[ "$app_manifest_identity" = "$(stat -Lc '%d:%i' "$app/manifest")" ] || {
  echo APPTAINER_PAIR_APP_MANIFEST_IDENTITY_CHANGED >&2; exit 4;
}
[ "$app_replay_identity" = "$(stat -Lc '%d:%i' "$app/replay")" ] || {
  echo APPTAINER_PAIR_APP_REPLAY_IDENTITY_CHANGED >&2; exit 4;
}
[ "$(python3 -c 'import json,sys; print(json.load(sys.stdin)["appDigest"])' <<<"$validation_json")" = \
  "$(python3 -c 'import json,sys; print(json.load(sys.stdin)["appDigest"])' <<<"$revalidated_json")" ] || {
  echo APPTAINER_PAIR_VALIDATION_CHANGED >&2; exit 4;
}
validation_json=$revalidated_json
home_target="/home/$(id -un)"
gpu_args=()
if [ "$gpu_count" -gt 0 ]; then gpu_args+=(--nv); fi

# Pin the exact Apptainer inode used by the final exec.  The descriptor is
# inherited through env and prevents a path replacement between validation and
# execution from changing the runtime binary.
apptainer_path_identity=$(stat -Lc '%d:%i' "$apptainer_bin")
exec 3<"$apptainer_bin" || { echo APPTAINER_PAIR_APPTAINER_OPEN_FAILED >&2; exit 4; }
[ "$(stat -Lc '%d:%i' /proc/self/fd/3)" = "$apptainer_path_identity" ] || {
  echo APPTAINER_PAIR_APPTAINER_IDENTITY_CHANGED >&2; exit 4;
}
apptainer_fd_sha=$(sha256sum /proc/self/fd/3 | awk '{print "sha256:"$1}')
[ "$apptainer_fd_sha" = "$recorded_apptainer_sha" ] || {
  echo APPTAINER_PAIR_APPTAINER_DIGEST_CHANGED >&2; exit 4;
}
base_path_identity=$(stat -Lc '%d:%i' "$base_sif")
exec 4<"$base_sif" || { echo APPTAINER_PAIR_BASE_SIF_OPEN_FAILED >&2; exit 4; }
[ "$(stat -Lc '%d:%i' /proc/self/fd/4)" = "$base_path_identity" ] || {
  echo APPTAINER_PAIR_BASE_SIF_IDENTITY_CHANGED >&2; exit 4;
}
base_fd_sha=$(sha256sum /proc/self/fd/4 | awk '{print "sha256:"$1}')
recorded_base_sha=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["baseSifSha256"])' <<<"$validation_json")
[ "$base_fd_sha" = "$recorded_base_sha" ] || {
  echo APPTAINER_PAIR_BASE_SIF_DIGEST_CHANGED >&2; exit 4;
}

if ! env -u APPTAINERENV_HOME -u APPTAINERENV_PWD -u SINGULARITYENV_HOME \
  -u SINGULARITYENV_PWD /proc/self/fd/3 exec --cleanenv --containall \
  /proc/self/fd/4 /bin/sh -c 'set -eu; test -x /opt/venv/bin/python; test -d /opt/ndn-base/lib; test -d /opt/onnxruntime/lib'; then
  echo APPTAINER_PAIR_BASE_RUNTIME_CONTRACT_FAILED >&2
  exit 4
fi

# Keep directory descriptors open through the final exec.  Binding these
# descriptors prevents a writable parent from replacing a validated child
# between the second validation and Apptainer's bind processing.
pin_dir() {
  local path="$1" output_var="$2" expected_identity="$3" fd
  [ -d "$path" ] || { echo APPTAINER_PAIR_BIND_NOT_DIRECTORY:"$path" >&2; exit 4; }
  exec {fd}<"$path" || { echo APPTAINER_PAIR_BIND_OPEN_FAILED:"$path" >&2; exit 4; }
  [ "$(stat -Lc '%d:%i' /proc/self/fd/$fd)" = "$expected_identity" ] || {
    echo APPTAINER_PAIR_BIND_IDENTITY_CHANGED:"$path" >&2; exit 4;
  }
  printf -v "$output_var" '%s' "$fd"
}
app_lib_fd=0; app_bin_fd=0; app_python_fd=0; app_manifest_fd=0; app_replay_fd=0
release_fd=0; models_fd=0; artifacts_fd=0; identity_fd=0; evidence_fd=0
release_identity=$(stat -Lc '%d:%i' "$release")
models_identity=$(stat -Lc '%d:%i' "$models")
artifacts_identity=$(stat -Lc '%d:%i' "$artifacts")
identity_dir_identity=$(stat -Lc '%d:%i' "$identity")
evidence_identity=$(stat -Lc '%d:%i' "$evidence")
pin_dir "$app/lib" app_lib_fd "$app_lib_identity"
pin_dir "$app/bin" app_bin_fd "$app_bin_identity"
pin_dir "$app/python" app_python_fd "$app_python_identity"
pin_dir "$app/manifest" app_manifest_fd "$app_manifest_identity"
pin_dir "$app/replay" app_replay_fd "$app_replay_identity"
pin_dir "$release" release_fd "$release_identity"
pin_dir "$models" models_fd "$models_identity"
pin_dir "$artifacts" artifacts_fd "$artifacts_identity"
pin_dir "$identity" identity_fd "$identity_dir_identity"
pin_dir "$evidence" evidence_fd "$evidence_identity"

# The APP paths are mounted explicitly.  Only the stable base-layer paths and
# the reviewed APP paths participate in lookup; an old complete-app tree in a
# base image is never a fallback.
env -u APPTAINERENV_HOME -u APPTAINERENV_PWD -u SINGULARITYENV_HOME \
  -u SINGULARITYENV_PWD /proc/self/fd/3 exec --cleanenv --containall --pwd /scratch \
  --home "/proc/self/fd/$scratch_fd/home:$home_target" "${gpu_args[@]}" \
  --env "SLURM_JOB_ID=$job_id,NDNSF_PAIR_APP_DIGEST=$(python3 -c 'import json,sys; print(json.load(sys.stdin)[\"appDigest\"])' <<<"$validation_json"),PATH=/opt/ndnsf-di/app/bin:/opt/venv/bin:/opt/ndn-base/bin:/usr/bin:/bin,LD_LIBRARY_PATH=/opt/ndnsf-di/app/lib:/opt/ndn-base/lib:/opt/onnxruntime/lib,PYTHONNOUSERSITE=1,PYTHONPATH=/opt/ndnsf-di/app/python,NDNSF_MODEL_ROOT=/models,NDNSF_ARTIFACT_ROOT=/artifacts" \
  --bind "/proc/self/fd/$app_lib_fd:/opt/ndnsf-di/app/lib:ro" \
  --bind "/proc/self/fd/$app_bin_fd:/opt/ndnsf-di/app/bin:ro" \
  --bind "/proc/self/fd/$app_python_fd:/opt/ndnsf-di/app/python:ro" \
  --bind "/proc/self/fd/$app_manifest_fd:/opt/ndnsf-di/app/manifest:ro" \
  --bind "/proc/self/fd/$app_replay_fd:/opt/ndnsf-di/app/replay:ro" \
  --bind "/proc/self/fd/$release_fd:/release:ro" \
  --bind "/proc/self/fd/$models_fd:/models:ro" \
  --bind "/proc/self/fd/$artifacts_fd:/artifacts:ro" \
  --bind "/proc/self/fd/$identity_fd:/identity:ro" \
  --bind "/proc/self/fd/$evidence_fd:/evidence:rw" \
  --bind "/proc/self/fd/$scratch_fd:/scratch:rw" \
  /proc/self/fd/4 "$@"
