#!/bin/bash
#SBATCH --job-name=spec166-qwen3
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=3
#SBATCH --nodelist=itiger07,itiger08,itiger09
#SBATCH --ntasks=3
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=32G
#SBATCH --gres=gpu:rtx_5000:1
#SBATCH --time=00:30:00
#SBATCH --output=/project/tma1/ndnsf-di/slurm/spec166-qwen-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/slurm/spec166-qwen-%j.err

set -Eeuo pipefail
umask 077
: "${SPEC166_RUN_ID:?SPEC166_RUN_ID_REQUIRED}"
: "${SPEC166_SUBMISSION_ID:?SPEC166_SUBMISSION_ID_REQUIRED}"
: "${SPEC166_SOURCE_ROOT:?SPEC166_SOURCE_ROOT_REQUIRED}"

readonly ROOT='/project/tma1/ndnsf-di'
readonly SPEC166_SIF="${ROOT}/releases/spec166-dcef2858c060/runtime.sif"
readonly SPEC166_INPUT="${ROOT}/inputs/spec166"
readonly SPEC166_SOURCE="${SPEC166_SOURCE_ROOT}"
readonly PARENT="${ROOT}/evidence/spec166/multinode"
readonly PARTIAL="${PARENT}/.${SPEC166_SUBMISSION_ID}.partial"
readonly FINAL="${PARENT}/${SPEC166_SUBMISSION_ID}"

comment=$(scontrol show job "$SLURM_JOB_ID" -o | tr ' ' '\n' | sed -n 's/^Comment=//p')
test "$comment" = "spec166:${SPEC166_SUBMISSION_ID}" || exit 4
test ! -e "$PARTIAL" && test ! -e "$FINAL" || exit 4
mkdir -p "$PARTIAL"
state=FAIL
finish()
{
  rc=$?
  trap - EXIT INT TERM
  rm -f -- "$PARTIAL/bootstrap-tokens.txt"
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
  (
    cd "$PARTIAL"
    find . -type f ! -name checksums.sha256 -print0 |
      sort -z | xargs -0 sha256sum
  ) > "$PARTIAL/checksums.sha256" 2>/dev/null || true
  if test "$state" = PASS && test "$rc" -eq 0; then
    mv "$PARTIAL" "$FINAL"
  fi
  exit "$rc"
}
trap finish EXIT INT TERM

(cd "$SPEC166_INPUT" && sha256sum -c checksums.sha256) \
  > "$PARTIAL/input-checksums.log"
(cd "$SPEC166_SOURCE" && sha256sum -c source-checksums.sha256) \
  > "$PARTIAL/source-checksums.log"
test -x "$SPEC166_SOURCE/multinode-rank.sh"
test -x "$SPEC166_SOURCE/multinode-rank-inner.sh"
test -f "$SPEC166_SOURCE/ndnsf_distributed_inference/app_sdk/client.py"
test -f "$SPEC166_SOURCE/ndnsf_distributed_inference/compatibility/manifest.json"
test -f "$SPEC166_SOURCE/analyze_multinode.py"
sif_sha=$(jq -r .sifSha256 \
  "$ROOT/releases/spec166-dcef2858c060/materialization.json")
sif_sha=${sif_sha#sha256:}
printf '%s  %s\n' "$sif_sha" "$SPEC166_SIF" | sha256sum -c - \
  > "$PARTIAL/sif-checksum.log"
mapfile -t nodes < <(scontrol show hostnames "$SLURM_JOB_NODELIST")
printf '%s\n' "${nodes[@]}" > "$PARTIAL/nodes.txt"
test "${nodes[*]}" = "itiger07 itiger08 itiger09"
port=$((20000 + SLURM_JOB_ID % 20000))
printf '%s\n' "$port" > "$PARTIAL/port.txt"

export SPEC166_SHARED="$PARTIAL"
export SPEC166_SIF SPEC166_INPUT SPEC166_SOURCE
export SPEC166_PORT="$port"
srun --kill-on-bad-exit=0 --label \
  --export=ALL,SPEC166_SHARED="$PARTIAL",SPEC166_SIF="$SPEC166_SIF",SPEC166_INPUT="$SPEC166_INPUT",SPEC166_SOURCE="$SPEC166_SOURCE",SPEC166_PORT="$port",SPEC166_RUN_ID="$SPEC166_RUN_ID" \
  "$SPEC166_SOURCE/multinode-rank.sh" \
  > "$PARTIAL/rank-step.log" 2> "$PARTIAL/rank-step.err"

test -f "$PARTIAL/rank-complete-0"
test -f "$PARTIAL/rank-complete-1"
test -f "$PARTIAL/rank-complete-2"
test ! -e "$PARTIAL/bootstrap-tokens.txt"
test -z "$(find "$PARTIAL" -type f \
  \( -name '*.privkey' -o -name 'bootstrap-tokens.txt' \) -print -quit)"
python3 "$SPEC166_SOURCE/analyze_multinode.py" \
  "$PARTIAL" "$PARTIAL/analysis.json" > "$PARTIAL/analysis.log"
grep -Fq SPEC166_MULTINODE_PASS "$PARTIAL/analysis.log"
state=PASS
