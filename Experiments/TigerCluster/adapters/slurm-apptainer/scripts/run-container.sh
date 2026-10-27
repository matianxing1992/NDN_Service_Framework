#!/bin/sh
set -eu

# The installed normalized caller owns role-specific paths; legacy invocations
# below retain their original Spec110/170 layout and admission defaults.
control_python=python3
if [ "${1:-}" = '--control-python' ]; then
  control_python=$2; shift 2
  case "$control_python" in /*) ;; *) echo DEPLOYMENT_CONTROL_PYTHON_REQUIRED >&2; exit 2 ;; esac
  [ -x "$control_python" ] || { echo DEPLOYMENT_CONTROL_PYTHON_REQUIRED >&2; exit 2; }
  [ "${1:-}" = '--deployment' ] || { echo DEPLOYMENT_ARGUMENT_REQUIRED >&2; exit 2; }
fi
if [ "${1:-}" = '--deployment' ]; then
  exec "$control_python" -I -m ndnsf_distributed_inference.ops.container "$@"
fi

sif=''; sif_sha=''; build_record=''; build_record_purpose='qualification'; project=''; scratch=''; identity=''; release=''; models=''; artifacts=''; evidence=''; gpu_count=''
while [ "$#" -gt 0 ]; do
  case "$1" in
    --sif) sif=$2; shift 2 ;;
    --sif-sha256) sif_sha=$2; shift 2 ;;
    --build-record) build_record=$2; shift 2 ;;
    --build-record-purpose) build_record_purpose=$2; shift 2 ;;
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
case "$build_record_purpose" in
  qualification) ;;
  deployment) [ -n "$build_record" ] || { echo APPTAINER_BUILD_RECORD_REQUIRED >&2; exit 4; } ;;
  *) echo APPTAINER_BUILD_RECORD_PURPOSE_INVALID >&2; exit 4 ;;
esac
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
    --expected-sha256 "$sif_sha" --metadata-only --purpose "$build_record_purpose"
  echo "SPEC170_BUILD_RECORD_METADATA_PASS record=$build_record sif=$sif_sha purpose=$build_record_purpose" >&2
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
scratch_input=Path(scratch)
scratch_path=scratch_input.resolve()
expected_scratch_prefix=f'ndnsf-di-{job}'
scratch_name=scratch_path.name
if (not scratch_input.is_absolute() or '..' in scratch_input.parts or
        scratch_path != scratch_input or scratch_input.is_symlink() or
        not str(scratch_path).startswith('/tmp/') or
        not (scratch_name == expected_scratch_prefix or
             scratch_name.startswith(expected_scratch_prefix + '-'))):
 raise SystemExit('APPTAINER_SCRATCH_SYMLINK_FORBIDDEN' if
                  scratch_path != scratch_input or scratch_input.is_symlink()
                  else 'APPTAINER_SCRATCH_INVALID')
if not scratch_path.is_dir():raise SystemExit('APPTAINER_SCRATCH_MISSING')
PY

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
scripts_dir=$(CDPATH= cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd)
local_sif=$(sh "$scripts_dir/stage-sif.sh" "$sif" "$sif_sha" "$source_sif_bytes")

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
