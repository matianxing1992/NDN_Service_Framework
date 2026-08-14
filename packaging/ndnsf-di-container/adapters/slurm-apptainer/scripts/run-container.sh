#!/bin/sh
set -eu

sif=''; sif_sha=''; project=''; scratch=''; identity=''; release=''; models=''; artifacts=''; evidence=''; gpu_count=''
while [ "$#" -gt 0 ]; do
  case "$1" in
    --sif) sif=$2; shift 2 ;;
    --sif-sha256) sif_sha=$2; shift 2 ;;
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
release=${release:-$project/releases}
models=${models:-$project/models}
artifacts=${artifacts:-$project/artifacts}
evidence=${evidence:-$project/evidence}

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
if not str(scratch_path).startswith('/tmp/') or scratch_path.name!=job:
 raise SystemExit('APPTAINER_SCRATCH_INVALID')
if not scratch_path.is_dir():raise SystemExit('APPTAINER_SCRATCH_MISSING')
PY

actual=sha256:$(sha256sum "$sif" | cut -d' ' -f1)
[ "$actual" = "$sif_sha" ] || { echo SIF_DIGEST_MISMATCH >&2; exit 4; }

# GPU runs use the literal `apptainer exec --cleanenv --nv` shape; CPU runs
# intentionally omit --nv so no host GPU libraries are injected.
gpu_args=''
if [ "$gpu_count" -gt 0 ]; then
  gpu_args='--nv'
fi

# shellcheck disable=SC2086
exec apptainer exec --cleanenv --containall --no-home $gpu_args \
  --env HOME=/scratch,PYTHONNOUSERSITE=1,PYTHONDONTWRITEBYTECODE=1,NDNSF_MODEL_ROOT=/models,NDNSF_ARTIFACT_ROOT=/artifacts \
  --bind "$scratch:/home/tma1:rw" \
  --bind "$release:/release:ro" \
  --bind "$models:/models:ro" \
  --bind "$artifacts:/artifacts:ro" \
  --bind "$identity:/identity:ro" \
  --bind "$evidence:/evidence:rw" \
  --bind "$scratch:/scratch:rw" \
  "$sif" "$@"
