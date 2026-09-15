#!/usr/bin/env bash
# Run a verified base SIF with an immutable external NDNSF-DI APP.
set -euo pipefail
umask 077

usage() {
  cat >&2 <<'EOF'
usage: run-sif-app.sh --base-sif PATH --app PATH --app-record PATH \
  --project PATH --scratch PATH --identity PATH --nfd-socket PATH \
  [--release-bind PATH] \
  [--models PATH] [--artifacts PATH] [--evidence PATH] [--gpu-count N] \
  [--apptainer PATH] [--expected-apptainer VERSION] [--local] -- COMMAND [ARGS...]

The default mode requires SLURM_JOB_ID.  --local is an explicit pre-Tiger
verification mode and keeps the same cleanenv/containall/mount contract.
EOF
  exit 2
}

base_sif=''; app=''; app_record=''; project=''; scratch=''; identity=''; nfd_socket=''
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
    --nfd-socket) nfd_socket="$2"; shift 2 ;;
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
  [ -z "$nfd_socket" ] || \
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
python3 - "$project" "$identity" "$release" "$models" "$artifacts" "$evidence" "$scratch" "$nfd_socket" <<'PY'
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
nfd_socket=$(readlink -m "$nfd_socket")
nfd_socket_dir=$(dirname -- "$nfd_socket")
nfd_socket_name=$(basename -- "$nfd_socket")
release=$(readlink -m "$release"); models=$(readlink -m "$models")
artifacts=$(readlink -m "$artifacts"); evidence=$(readlink -m "$evidence")
[ -f "$base_sif" ] || { echo APPTAINER_PAIR_BASE_SIF_MISSING >&2; exit 4; }
if [ ! -f "$apptainer_bin" ] || [ ! -x "$apptainer_bin" ]; then
  echo APPTAINER_PAIR_APPTAINER_NOT_REGULAR >&2; exit 4;
fi
[ -d "$app" ] || { echo APPTAINER_PAIR_APP_MISSING >&2; exit 4; }
[ -f "$app_record" ] || { echo APPTAINER_PAIR_APP_RECORD_MISSING >&2; exit 4; }
[ -d "$identity" ] || { echo APPTAINER_PAIR_IDENTITY_MISSING >&2; exit 4; }
[ -d "$identity/.ndn" ] || { echo APPTAINER_PAIR_IDENTITY_STORE_MISSING >&2; exit 4; }
[ -f "$identity/.ndn/pib.db" ] || { echo APPTAINER_PAIR_IDENTITY_PIB_MISSING >&2; exit 4; }
[ -d "$identity/.ndn/ndnsec-key-file" ] || { echo APPTAINER_PAIR_IDENTITY_TPM_MISSING >&2; exit 4; }
if find "$identity/.ndn" -type l -print -quit | grep -q .; then
  echo APPTAINER_PAIR_IDENTITY_SYMLINK_FORBIDDEN >&2; exit 4
fi
if find "$identity/.ndn" \! -type f \! -type d -print -quit | grep -q .; then
  echo APPTAINER_PAIR_IDENTITY_SPECIAL_FILE >&2; exit 4
fi
if find "$identity/.ndn/ndnsec-key-file" -mindepth 1 -maxdepth 1 \
  \( \! -type f -o \! -name '*.privkey' \) -print -quit | grep -q .; then
  echo APPTAINER_PAIR_IDENTITY_TPM_FILE_INVALID >&2; exit 4
fi
[ "$(find "$identity/.ndn/ndnsec-key-file" -mindepth 1 -maxdepth 1 \
  -type f -name '*.privkey' -printf x | wc -c)" -eq 1 ] || {
  echo APPTAINER_PAIR_IDENTITY_TPM_KEY_COUNT_INVALID >&2; exit 4;
}
[ -S "$nfd_socket" ] || { echo APPTAINER_PAIR_NFD_SOCKET_MISSING >&2; exit 4; }
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
[ "$(stat -c '%u' "$nfd_socket_dir")" = "$(id -u)" ] || {
  echo APPTAINER_PAIR_NFD_SOCKET_DIR_OWNER_MISMATCH >&2; exit 4;
}
nfd_socket_dir_mode=$(stat -c '%a' "$nfd_socket_dir")
if (( 0$nfd_socket_dir_mode & 077 )); then
  echo APPTAINER_PAIR_NFD_SOCKET_DIR_NOT_PRIVATE >&2; exit 4;
fi
[ "$(stat -c '%u' "$nfd_socket")" = "$(id -u)" ] || {
  echo APPTAINER_PAIR_NFD_SOCKET_OWNER_MISMATCH >&2; exit 4;
}
if [ "$local_mode" = 0 ]; then
  case "$nfd_socket_dir" in
    "/tmp/ndnsf-di-$job_id"|"/tmp/ndnsf-di-$job_id"/*) ;;
    *) echo APPTAINER_PAIR_NFD_SOCKET_NOT_JOB_SCOPED >&2; exit 4 ;;
  esac
else
  case "$nfd_socket_dir" in
    /tmp/ndnsf-di-local-*) ;;
    *) echo APPTAINER_PAIR_NFD_SOCKET_NOT_LOCAL_SCOPED >&2; exit 4 ;;
  esac
fi
if find "$nfd_socket_dir" -mindepth 1 -maxdepth 1 \
  ! -name "$nfd_socket_name" -print -quit | grep -q .; then
  echo APPTAINER_PAIR_NFD_SOCKET_DIR_NOT_DEDICATED >&2; exit 4
fi
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

# Keep directory descriptors open through the final command.  Binding these
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
release_fd=0; models_fd=0; artifacts_fd=0; identity_fd=0; nfd_socket_dir_fd=0; evidence_fd=0
release_identity=$(stat -Lc '%d:%i' "$release")
models_identity=$(stat -Lc '%d:%i' "$models")
artifacts_identity=$(stat -Lc '%d:%i' "$artifacts")
identity_dir_identity=$(stat -Lc '%d:%i' "$identity")
nfd_socket_identity=$(stat -Lc '%d:%i' "$nfd_socket")
nfd_socket_dir_identity=$(stat -Lc '%d:%i' "$nfd_socket_dir")
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
pin_dir "$nfd_socket_dir" nfd_socket_dir_fd "$nfd_socket_dir_identity"
pin_dir "$evidence" evidence_fd "$evidence_identity"

# The identity input is a read-only source.  Copy its single role store into
# this run's private HOME so Runtime can use paired locators without opening a
# shared SQLite database or depending on a mutable host HOME.
mkdir "$scratch/home/.ndn"
cp -a --no-preserve=ownership "/proc/self/fd/$identity_fd/.ndn/." "$scratch/home/.ndn/"
chmod 700 "$scratch/home/.ndn"
if [ -L "$scratch/home/.ndn/pib.db" ] || [ ! -f "$scratch/home/.ndn/pib.db" ]; then
  echo APPTAINER_PAIR_STAGED_PIB_INVALID >&2; exit 4;
fi
if [ -L "$scratch/home/.ndn/ndnsec-key-file" ] || [ ! -d "$scratch/home/.ndn/ndnsec-key-file" ]; then
  echo APPTAINER_PAIR_STAGED_TPM_INVALID >&2; exit 4;
fi
if find "$scratch/home/.ndn/ndnsec-key-file" -mindepth 1 -maxdepth 1 \
  \( \! -type f -o \! -name '*.privkey' \) -print -quit | grep -q .; then
  echo APPTAINER_PAIR_STAGED_TPM_FILE_INVALID >&2; exit 4
fi
[ "$(find "$scratch/home/.ndn/ndnsec-key-file" -mindepth 1 -maxdepth 1 \
  -type f -name '*.privkey' -printf x | wc -c)" -eq 1 ] || {
  echo APPTAINER_PAIR_STAGED_TPM_KEY_COUNT_INVALID >&2; exit 4;
}
identity_digest=$(python3 - "$identity_fd" "$scratch/home/.ndn" <<'PY'
import hashlib
import stat
import sys
from pathlib import Path

source_root = Path("/proc/self/fd") / sys.argv[1] / ".ndn"
target_root = Path(sys.argv[2])

def tree_digest(root):
    digest = hashlib.sha256()
    entries = sorted(root.rglob("*"), key=lambda path: path.relative_to(root).as_posix())
    for path in entries:
        relative = path.relative_to(root).as_posix().encode()
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode) or not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)):
            raise SystemExit("APPTAINER_PAIR_IDENTITY_DIGEST_INPUT_INVALID")
        digest.update(relative + b"\0" + str(stat.S_IMODE(info.st_mode)).encode() + b"\0")
        if stat.S_ISREG(info.st_mode):
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
        digest.update(b"\0")
    return "sha256:" + digest.hexdigest()

source_digest = tree_digest(source_root)
target_digest = tree_digest(target_root)
if source_digest != target_digest:
    raise SystemExit("APPTAINER_PAIR_IDENTITY_DIGEST_MISMATCH")
print(source_digest)
PY
)
printf 'APPTAINER_PAIR_IDENTITY_DIGEST=%s\n' "$identity_digest" >&2
python3 - "$scratch/home/.ndn/pib.db" "$home_target/.ndn" <<'PY'
import sqlite3
import sys

database, locator_root = sys.argv[1:]
locator = "tpm-file:" + locator_root
with sqlite3.connect(database) as connection:
    rows = connection.execute("SELECT tpm_locator FROM tpmInfo").fetchall()
    if len(rows) != 1:
        raise SystemExit("APPTAINER_PAIR_TPM_INFO_INVALID")
    connection.execute("UPDATE tpmInfo SET tpm_locator = ?", (locator,))
    connection.commit()
    observed = connection.execute("SELECT tpm_locator FROM tpmInfo").fetchone()[0]
    if observed != locator:
        raise SystemExit("APPTAINER_PAIR_TPM_LOCATOR_REWRITE_FAILED")
PY
[ "$nfd_socket_identity" = "$(stat -Lc '%d:%i' "$nfd_socket")" ] || {
  echo APPTAINER_PAIR_NFD_SOCKET_IDENTITY_CHANGED >&2; exit 4;
}

# The APP paths are mounted explicitly.  Only the stable base-layer paths and
# the reviewed APP paths participate in lookup; an old complete-app tree in a
# base image is never a fallback.
set +e
# shellcheck disable=SC2016 # The single-quoted wrapper runs inside the SIF.
env -u APPTAINERENV_HOME -u APPTAINERENV_PWD -u SINGULARITYENV_HOME \
  -u SINGULARITYENV_PWD /proc/self/fd/3 exec --cleanenv --containall --pwd /scratch \
  --home "/proc/self/fd/$scratch_fd/home:$home_target" "${gpu_args[@]}" \
  --env "SLURM_JOB_ID=$job_id,NDNSF_PAIR_APP_DIGEST=$(python3 -c 'import json,sys; print(json.load(sys.stdin)[\"appDigest\"])' <<<"$validation_json"),PATH=/opt/ndnsf-di/app/bin:/opt/venv/bin:/opt/ndn-base/bin:/usr/bin:/bin,LD_LIBRARY_PATH=/opt/ndnsf-di/app/lib:/opt/ndn-base/lib:/opt/onnxruntime/lib,PYTHONNOUSERSITE=1,PYTHONPATH=/opt/ndnsf-di/app/python,NDNSF_MODEL_ROOT=/models,NDNSF_ARTIFACT_ROOT=/artifacts,NDN_CLIENT_TRANSPORT=unix:///tmp/ndnsf-di-nfd/$nfd_socket_name,NDN_CLIENT_PIB=pib-sqlite3:$home_target/.ndn,NDN_CLIENT_TPM=tpm-file:$home_target/.ndn" \
  --bind "/proc/self/fd/$app_lib_fd:/opt/ndnsf-di/app/lib:ro" \
  --bind "/proc/self/fd/$app_bin_fd:/opt/ndnsf-di/app/bin:ro" \
  --bind "/proc/self/fd/$app_python_fd:/opt/ndnsf-di/app/python:ro" \
  --bind "/proc/self/fd/$app_manifest_fd:/opt/ndnsf-di/app/manifest:ro" \
  --bind "/proc/self/fd/$app_replay_fd:/opt/ndnsf-di/app/replay:ro" \
  --bind "/proc/self/fd/$release_fd:/release:ro" \
  --bind "/proc/self/fd/$models_fd:/models:ro" \
  --bind "/proc/self/fd/$artifacts_fd:/artifacts:ro" \
  --bind "/proc/self/fd/$identity_fd:/identity:ro" \
  --bind "/proc/self/fd/$nfd_socket_dir_fd:/tmp/ndnsf-di-nfd:ro" \
  --bind "/proc/self/fd/$evidence_fd:/evidence:rw" \
  --bind "/proc/self/fd/$scratch_fd:/scratch:rw" \
  /proc/self/fd/4 /bin/sh -c '
    set -eu
    socket_path="$1"
    expected_identity="$2"
    shift 2
    test -S "$socket_path"
    test "$(stat -Lc "%d:%i" "$socket_path")" = "$expected_identity"
    exec "$@"
  ' sh "/tmp/ndnsf-di-nfd/$nfd_socket_name" "$nfd_socket_identity" "$@"
app_status=$?
set -e
exit "$app_status"
