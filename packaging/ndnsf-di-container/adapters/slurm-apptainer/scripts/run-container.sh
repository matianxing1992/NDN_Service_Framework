#!/bin/sh
set -eu

sif=''; sif_sha=''; build_record=''; project=''; scratch=''; identity=''; release=''; models=''; artifacts=''; evidence=''; gpu_count=''
while [ "$#" -gt 0 ]; do
  case "$1" in
    --sif) sif=$2; shift 2 ;;
    --sif-sha256) sif_sha=$2; shift 2 ;;
    --build-record) build_record=$2; shift 2 ;;
    --project) project=$2; shift 2 ;;
    --scratch) scratch=$2; shift 2 ;;
    --identity) identity=$2; shift 2 ;;
    --release-bind) release=$2; shift 2 ;;
    --models) models=$2; shift 2 ;;
    --artifacts) artifacts=$2; shift 2 ;;
    --evidence) evidence=$2; shift 2 ;;
    --gpu-count) gpu_count=$2; shift 2 ;;
    --) shift; break ;;
    *) echo "APPTAINER_RUN_ARGUMENT_INVALID:$1" >&2; exit 2 ;;
  esac
done
[ -n "${SLURM_JOB_ID:-}" ] || { echo APPTAINER_RUN_REQUIRES_SLURM >&2; exit 3; }
if [ -z "$sif" ] || [ -z "$sif_sha" ] || [ -z "$project" ] || [ -z "$scratch" ] || [ -z "$identity" ]; then
  echo APPTAINER_RUN_REQUIRED_PATH_MISSING >&2; exit 2;
fi
[ -n "$gpu_count" ] || gpu_count=${SLURM_GPUS_ON_NODE:-0}
case "$gpu_count" in
  ''|*[!0-9]*) echo APPTAINER_GPU_COUNT_INVALID >&2; exit 2 ;;
esac
[ "$#" -gt 0 ] || { echo WORKLOAD_REQUIRED >&2; exit 2; }
[ -f "$sif" ] || { echo SIF_MISSING >&2; exit 4; }
release=${release:-$project/releases}
models=${models:-$project/models}
artifacts=${artifacts:-$project/artifacts}
evidence=${evidence:-$project/evidence}

# A SIF digest alone cannot prove how its native Python extension was built.
# Formal Spec170 jobs pass the build record explicitly; validating it here
# rejects legacy v2 records (including revoked r13) before a multi-gigabyte
# image is copied to node-local scratch.  The validator is deliberately pure
# Python and never imports the host checkout's ndnsf module.
if [ -n "$build_record" ]; then
  build_record=$(readlink -f "$build_record")
  project_root=$(readlink -f "$project")
  case "$build_record" in
    "$project_root"/*) ;;
    *) echo APPTAINER_BUILD_RECORD_OUTSIDE_PROJECT >&2; exit 4 ;;
  esac
  build_record_validator="$project/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/validate-local-sif-build-record.py"
  [ -f "$build_record_validator" ] || { echo APPTAINER_BUILD_RECORD_VALIDATOR_MISSING >&2; exit 4; }
  # The SIF lives on shared project storage.  A full hash here can block in
  # uninterruptible NFS I/O for many minutes before the node-local cache is
  # even considered.  Validate the immutable record and source size first;
  # the staged node-local copy is hashed below immediately before execution.
  python3 "$build_record_validator" --record "$build_record" --sif "$sif" \
    --expected-sha256 "$sif_sha" --metadata-only
  echo "SPEC170_BUILD_RECORD_METADATA_PASS record=$build_record sif=$sif_sha" >&2
fi

python3 - "$project" "$release" "$models" "$artifacts" "$identity" "$evidence" "$scratch" "$SLURM_JOB_ID" <<'PY'
from pathlib import Path
import os,sys
project,release,models,artifacts,identity,evidence,scratch,job=sys.argv[1:]
root=Path(project).resolve();allow_test=os.environ.get('NDNSF_SPEC110_ALLOW_TEST_ROOT')=='1'
if (not allow_test and not str(root).startswith('/project/')) or not str(root).endswith('/ndnsf-di'):
 raise SystemExit('APPTAINER_PROJECT_ROOT_INVALID')
for label,value in [('release',release),('models',models),('artifacts',artifacts),('identity',identity),('evidence',evidence)]:
 path=Path(value).resolve()
 try:path.relative_to(root)
 except ValueError:raise SystemExit('APPTAINER_BIND_OUTSIDE_PROJECT:'+label)
 if not path.exists():raise SystemExit('APPTAINER_BIND_MISSING:'+label)
scratch_path=Path(scratch).resolve()
expected_scratch_name=f'ndnsf-di-{job}'
if not str(scratch_path).startswith('/tmp/') or scratch_path.name!=expected_scratch_name:
 raise SystemExit('APPTAINER_SCRATCH_INVALID')
if not scratch_path.is_dir():raise SystemExit('APPTAINER_SCRATCH_MISSING')
PY

# Hashing a multi-gigabyte SIF directly on project storage can exceed a short
# Slurm preflight window. Stage it to node-local storage and retain one
# verified copy per immutable digest. A job-specific scratch path alone would
# force every D0/D1/D2 case on the same node to copy the same 4+ GB image again.
sif_cache_key=${sif_sha#sha256:}
case "$sif_cache_key" in
  ''|*[!0-9a-fA-F]*) echo SIF_DIGEST_INVALID >&2; exit 4 ;;
esac
sif_cache_dir="${NDNSF_SIF_CACHE_DIR:-/tmp/ndnsf-di-sif-cache}/$sif_cache_key"
mkdir -p "$sif_cache_dir"
local_sif="$sif_cache_dir/runtime.sif"
if [ -n "$build_record" ]; then
  # The metadata-only validator deliberately does not stat the shared SIF.
  # Reuse the immutable byte count from the record for cache admission; the
  # staged copy's digest below remains the execution gate.
  source_sif_bytes=$(python3 - "$build_record" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as source:
    record = json.load(source)
print(record["sif"]["bytes"])
PY
  )
else
  source_sif_bytes=$(stat -Lc '%s' "$sif")
fi
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

# GPU runs use the literal `apptainer exec --cleanenv --nv` shape; CPU runs
# intentionally omit --nv so no host GPU libraries are injected.
gpu_args=''
if [ "$gpu_count" -gt 0 ]; then
  gpu_args='--nv'
fi
mkdir -p "$scratch/home"
home_target="/home/$(id -un)"

# shellcheck disable=SC2086
# Do not let host APPTAINERENV_* variables override the isolated HOME/PWD
# contract.  Slurm/login environments may export APPTAINERENV_HOME pointing at
# a host directory that is not writable or not mounted in the SIF.
exec env -u APPTAINERENV_HOME -u APPTAINERENV_PWD -u SINGULARITYENV_HOME \
  -u SINGULARITYENV_PWD apptainer exec --cleanenv --containall --pwd /scratch \
  --home "$scratch/home:$home_target" $gpu_args \
  --env SLURM_JOB_ID="$SLURM_JOB_ID",PYTHONNOUSERSITE=1,PYTHONDONTWRITEBYTECODE=1,NDNSF_MODEL_ROOT=/models,NDNSF_ARTIFACT_ROOT=/artifacts \
  --bind "$release:/release:ro" \
  --bind "$models:/models:ro" \
  --bind "$artifacts:/artifacts:ro" \
  --bind "$identity:/identity:ro" \
  --bind "$evidence:/evidence:rw" \
  --bind "$scratch:/scratch:rw" \
  "$local_sif" "$@"
