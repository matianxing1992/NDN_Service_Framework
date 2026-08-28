#!/bin/bash
#SBATCH --job-name=spec166-gpu-ref
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=1
#SBATCH --nodelist=itiger07
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --gres=gpu:rtx_5000:1
#SBATCH --time=00:10:00
#SBATCH --output=/project/tma1/ndnsf-di/slurm/spec166-gpu-ref-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/slurm/spec166-gpu-ref-%j.err

set -Eeuo pipefail
umask 077

: "${SPEC166_RUN_ID:?SPEC166_RUN_ID_REQUIRED}"
: "${SPEC166_SUBMISSION_ID:?SPEC166_SUBMISSION_ID_REQUIRED}"
: "${SPEC166_SOURCE_ROOT:?SPEC166_SOURCE_ROOT_REQUIRED}"

readonly ROOT='/project/tma1/ndnsf-di'
readonly IMAGE="${ROOT}/releases/spec166-dcef2858c060/runtime.sif"
readonly INPUT="${ROOT}/inputs/spec166"
readonly SOURCE="${SPEC166_SOURCE_ROOT}"
readonly PARTIAL="${ROOT}/evidence/spec166/standalone/.${SPEC166_SUBMISSION_ID}.partial"
readonly FINAL="${ROOT}/evidence/spec166/standalone/${SPEC166_SUBMISSION_ID}"

comment=$(scontrol show job "$SLURM_JOB_ID" -o | tr ' ' '\n' | sed -n 's/^Comment=//p')
test "$comment" = "spec166:${SPEC166_SUBMISSION_ID}" || exit 4
test ! -e "$PARTIAL" && test ! -e "$FINAL" || exit 4
mkdir -p "$PARTIAL"
state=FAIL
finish()
{
  rc=$?
  trap - EXIT INT TERM
  python3 - "$PARTIAL/result-terminal.json" "$state" "$rc" \
    "$SLURM_JOB_ID" "$SPEC166_SUBMISSION_ID" <<'PY' || true
import json,sys
path,state,rc,job,submission=sys.argv[1:]
with open(path,'w',encoding='utf-8') as stream:
  json.dump({'schemaVersion':'spec166-slurm-terminal-v1','state':state,
             'exitCode':int(rc),'jobId':job,'submissionId':submission},
            stream,indent=2,sort_keys=True)
  stream.write('\n')
PY
  if [ "$state" = PASS ] && [ "$rc" -eq 0 ]; then
    mv "$PARTIAL" "$FINAL"
  fi
  exit "$rc"
}
trap finish EXIT INT TERM

nvidia-smi -L > "$PARTIAL/nvidia-smi.txt"
nvidia-smi --query-gpu=name,uuid,memory.total,driver_version \
  --format=csv,noheader >> "$PARTIAL/nvidia-smi.txt"
hostname > "$PARTIAL/hostname.txt"
(cd "$SOURCE" && sha256sum -c source-checksums.sha256) \
  > "$PARTIAL/source-checksums.log"

apptainer exec --nv --cleanenv --containall \
  --env PYTHONNOUSERSITE=1,PYTHONDONTWRITEBYTECODE=1 \
  --env PYTHONPATH=/source/llm_pipeline:/opt/ndnsf-app/python \
  --bind "$SOURCE:/source:ro" \
  --bind "$INPUT:/input:ro" \
  --bind "$PARTIAL:/output:rw" \
  "$IMAGE" \
  /opt/venv/bin/python /source/run_standalone_gpu_reference.py \
    --campaign /input/generation-campaign.json \
    --workload /input/workload.json \
    --service-manifest /input/qwen-onnx-service-manifest.json \
    --artifacts /input/artifacts \
    --output /output/result.json \
    --run-id "$SPEC166_RUN_ID" \
  > "$PARTIAL/run.log" 2>&1

grep -Fq SPEC166_STANDALONE_GPU_PASS "$PARTIAL/run.log"
python3 - "$PARTIAL/result.json" <<'PY'
import json,sys
doc=json.load(open(sys.argv[1],encoding='utf-8'))
assert doc['state']=='PASS'
assert doc['requestCount']==8 and doc['warmupCount']==2
assert doc['measuredCount']==6 and doc['tokensPerRequest']==8
assert all(row['exactMatch'] for row in doc['requests'])
assert all(stage['device']=='cuda:0' and stage['cpuFallback']==0
           for stage in doc['placements'])
assert all(stage['profilePolicy']=='PASS'
           and stage['profile']['nodeEventCount'] > 0
           for stage in doc['placements'])
PY
(
  cd "$PARTIAL"
  find . -maxdepth 1 -type f ! -name checksums.sha256 -print0 |
    sort -z | xargs -0 sha256sum
) > "$PARTIAL/checksums.sha256"
state=PASS
