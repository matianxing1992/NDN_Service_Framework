#!/bin/sh
# Shared digest-locked staging for legacy and normalized container execution.
set -eu
umask 077
[ "$#" -eq 3 ] || { echo SIF_STAGE_ARGUMENTS_INVALID >&2; exit 2; }
sif=$1
sif_sha=$2
source_sif_bytes=$3
case "$source_sif_bytes" in
  ''|*[!0-9]*) echo SIF_SIZE_INVALID >&2; exit 4 ;;
esac
# Hashing a multi-gigabyte SIF directly on project storage can exceed a short
# Slurm preflight window. Stage it to node-local storage and retain one
# verified copy per immutable digest. A job-specific scratch path alone would
# force every D0/D1/D2 case on the same node to copy the same 4+ GB image again.
sif_cache_key=${sif_sha#sha256:}
case "$sif_cache_key" in
  ''|*[!0-9a-fA-F]*) echo SIF_DIGEST_INVALID >&2; exit 4 ;;
esac
sif_cache_root="${NDNSF_SIF_CACHE_DIR:-/tmp/ndnsf-di-sif-cache-$(id -u)}"
# The digest is immutable, but a shared cache parent is still writable by
# other cluster users on many /tmp configurations. Scope the cache below the
# numeric uid and make that directory private before opening the lock; this
# prevents another user from planting a stage.lock symlink or replacing a
# partially staged image for this job.
sif_cache_user_root="$sif_cache_root/u$(id -u)"
sif_cache_dir="$sif_cache_user_root/$sif_cache_key"
# Check ownership before chmod or opening a lock. In particular, following an
# existing uid-directory symlink must not redirect writes outside this cache.
python3 -I - "$sif_cache_user_root" "$sif_cache_dir" <<'PY'
import os, stat, sys
from pathlib import Path
for value in sys.argv[1:]:
    path = Path(value)
    if not path.is_absolute() or path.resolve() != path:
        raise SystemExit('SIF_CACHE_PATH_ALIAS_FORBIDDEN')
    path.mkdir(parents=True, mode=0o700, exist_ok=True)
    info = path.lstat()
    if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or
            stat.S_IMODE(info.st_mode) != 0o700):
        raise SystemExit('SIF_CACHE_DIRECTORY_NOT_PRIVATE')
    for parent in path.parents:
        item = parent.lstat()
        if item.st_uid not in (0, os.getuid()) or (item.st_mode & 0o022 and not item.st_mode & stat.S_ISVTX):
            raise SystemExit('SIF_CACHE_ANCESTOR_UNTRUSTED')
    for name in ('stage.lock', 'runtime.sif'):
        target = path / name
        if target.exists() or target.is_symlink():
            item = target.lstat()
            if not stat.S_ISREG(item.st_mode) or item.st_uid != os.getuid() or item.st_nlink != 1:
                raise SystemExit('SIF_CACHE_ENTRY_INVALID')
PY
local_sif="$sif_cache_dir/runtime.sif"
command -v flock >/dev/null 2>&1 || { echo SIF_CACHE_FLOCK_MISSING >&2; exit 4; }
exec 9>"$sif_cache_dir/stage.lock"
flock 9
# A previous cancelled copy is never a candidate.  The digest-scoped lock
# makes this cleanup safe even when several jobs request the same SIF.
find "$sif_cache_dir" -maxdepth 1 -type f -name 'runtime.sif.tmp.*' -delete
if [ "$sif" != "$local_sif" ]; then
  if [ ! -f "$local_sif" ] || [ "$(stat -c '%s' "$local_sif" 2>/dev/null || echo 0)" -ne "$source_sif_bytes" ]; then
    printf 'SIF_STAGE_START source=%s target=%s bytes=%s\n' "$sif" "$local_sif" "$source_sif_bytes" >&2
    tmp_sif="$sif_cache_dir/runtime.sif.tmp.$$"
    rm -f "$tmp_sif"
    cp --reflink=auto "$sif" "$tmp_sif"
    mv -f "$tmp_sif" "$local_sif"
    printf 'SIF_STAGE_COMPLETE target=%s bytes=%s\n' "$local_sif" "$(stat -c '%s' "$local_sif")" >&2
  fi
else
  local_sif="$sif"
fi
actual=sha256:$(sha256sum "$local_sif" | cut -d' ' -f1)
if [ "$actual" != "$sif_sha" ] && [ "$sif" != "$local_sif" ]; then
  # A same-sized but corrupt cache entry must not poison every later job.
  printf 'SIF_CACHE_REPAIR source=%s target=%s observed=%s expected=%s\n' \
    "$sif" "$local_sif" "$actual" "$sif_sha" >&2
  rm -f "$local_sif"
  tmp_sif="$sif_cache_dir/runtime.sif.tmp.$$"
  cp --reflink=auto "$sif" "$tmp_sif"
  mv -f "$tmp_sif" "$local_sif"
  actual=sha256:$(sha256sum "$local_sif" | cut -d' ' -f1)
fi
flock -u 9
[ "$actual" = "$sif_sha" ] || { echo SIF_DIGEST_MISMATCH >&2; exit 4; }
printf 'SIF_STAGE_VERIFY_PASS source=%s staged=%s digest=%s\n' "$sif" "$local_sif" "$actual" >&2

printf '%s\n' "$local_sif"
