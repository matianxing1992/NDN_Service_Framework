#!/bin/sh
set -eu

sif=''; sif_sha=''; project=''; scratch=''; identity=''; release=''; models=''; evidence=''
while [ "$#" -gt 0 ]; do
  case "$1" in
    --sif) sif=$2; shift 2 ;;
    --sif-sha256) sif_sha=$2; shift 2 ;;
    --project) project=$2; shift 2 ;;
    --scratch) scratch=$2; shift 2 ;;
    --identity) identity=$2; shift 2 ;;
    --release-bind) release=$2; shift 2 ;;
    --models) models=$2; shift 2 ;;
    --evidence) evidence=$2; shift 2 ;;
    --) shift; break ;;
    *) echo "APPTAINER_RUN_ARGUMENT_INVALID:$1" >&2; exit 2 ;;
  esac
done
[ -n "${SLURM_JOB_ID:-}" ] || { echo APPTAINER_RUN_REQUIRES_SLURM >&2; exit 3; }
[ -n "$sif" ] && [ -n "$sif_sha" ] && [ -n "$project" ] && [ -n "$scratch" ] && [ -n "$identity" ] || {
  echo APPTAINER_RUN_REQUIRED_PATH_MISSING >&2; exit 2;
}
[ "$#" -gt 0 ] || { echo WORKLOAD_REQUIRED >&2; exit 2; }
release=${release:-$project/releases}
models=${models:-$project/models}
evidence=${evidence:-$project/evidence}

python3 - "$project" "$release" "$models" "$identity" "$evidence" "$scratch" "$SLURM_JOB_ID" <<'PY'
from pathlib import Path
import os,sys
project,release,models,identity,evidence,scratch,job=sys.argv[1:]
root=Path(project).resolve();allow_test=os.environ.get('NDNSF_SPEC110_ALLOW_TEST_ROOT')=='1'
if (not allow_test and not str(root).startswith('/project/')) or not str(root).endswith('/ndnsf-di'):
 raise SystemExit('APPTAINER_PROJECT_ROOT_INVALID')
for label,value in [('release',release),('models',models),('identity',identity),('evidence',evidence)]:
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

exec apptainer exec --cleanenv --containall --no-home --nv \
  --env HOME=/scratch,PYTHONNOUSERSITE=1,PYTHONDONTWRITEBYTECODE=1 \
  --bind "$release:/release:ro" \
  --bind "$models:/models:ro" \
  --bind "$identity:/identity:ro" \
  --bind "$evidence:/evidence:rw" \
  --bind "$scratch:/scratch:rw" \
  "$sif" "$@"
