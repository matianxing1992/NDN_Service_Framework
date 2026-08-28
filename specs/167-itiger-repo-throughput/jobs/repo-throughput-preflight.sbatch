#!/bin/bash
#SBATCH --job-name=spec167-repo-preflight
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=2
#SBATCH --nodelist=itiger07,itiger08
#SBATCH --ntasks=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=16G
#SBATCH --time=00:10:00
#SBATCH --output=/project/tma1/ndnsf-di/slurm/spec167-preflight-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/slurm/spec167-preflight-%j.err

set -Eeuo pipefail
umask 077
: "${SPEC167_SUBMISSION_ID:?SPEC167_SUBMISSION_ID_REQUIRED}"
: "${SPEC167_SOURCE_ROOT:?SPEC167_SOURCE_ROOT_REQUIRED}"

readonly ROOT=/project/tma1/ndnsf-di
readonly SIF="$ROOT/releases/spec166-dcef2858c060/runtime.sif"
readonly SOURCE="$SPEC167_SOURCE_ROOT"
readonly PARENT="$ROOT/evidence/spec167/preflight"
readonly PARTIAL="$PARENT/.${SPEC167_SUBMISSION_ID}.partial"
readonly FINAL="$PARENT/${SPEC167_SUBMISSION_ID}"

comment=$(scontrol show job "$SLURM_JOB_ID" -o | tr ' ' '\n' | sed -n 's/^Comment=//p')
test "$comment" = "spec167:${SPEC167_SUBMISSION_ID}"
test ! -e "$PARTIAL" && test ! -e "$FINAL"
mkdir -p "$PARTIAL"
state=FAIL
finish()
{
  rc=$?
  trap - EXIT INT TERM
  python3 - "$PARTIAL/result-terminal.json" "$state" "$rc" "$SLURM_JOB_ID" "$SPEC167_SUBMISSION_ID" <<'PY' || true
import json,sys
path,state,rc,job,submission=sys.argv[1:]
with open(path,'w',encoding='utf-8') as stream:
    json.dump({'schemaVersion':1,'state':state,'exitCode':int(rc),'jobId':job,
               'submissionId':submission},stream,indent=2,sort_keys=True)
    stream.write('\n')
PY
  (cd "$PARTIAL" && find . -type f ! -name checksums.sha256 -print0 | sort -z | xargs -0 sha256sum) > "$PARTIAL/checksums.sha256" || true
  if test "$state" = PASS && test "$rc" -eq 0; then mv "$PARTIAL" "$FINAL"; fi
  exit "$rc"
}
trap finish EXIT INT TERM

mkdir -p "$ROOT/slurm" "$PARENT"
(cd "$SOURCE" && sha256sum -c source-checksums.sha256) > "$PARTIAL/source-checksums.log"
sif_sha=$(jq -r .sifSha256 "$ROOT/releases/spec166-dcef2858c060/materialization.json")
sif_sha=${sif_sha#sha256:}
printf '%s  %s\n' "$sif_sha" "$SIF" | sha256sum -c - > "$PARTIAL/sif-checksum.log"
test -x "$SOURCE/specs/167-itiger-repo-throughput/jobs/rank.sh"
test -x "$SOURCE/specs/167-itiger-repo-throughput/jobs/rank-inner.sh"
mapfile -t nodes < <(scontrol show hostnames "$SLURM_JOB_NODELIST")
printf '%s\n' "${nodes[@]}" > "$PARTIAL/nodes.txt"
test "${nodes[*]}" = "itiger07 itiger08"
port=$((22000 + SLURM_JOB_ID % 15000))
export SPEC167_SHARED="$PARTIAL" SPEC167_SIF="$SIF" SPEC167_SOURCE="$SOURCE"
export SPEC167_PORT="$port" SPEC167_MODE=preflight
srun --kill-on-bad-exit=1 --label \
  --export=ALL,SPEC167_SHARED,SPEC167_SIF,SPEC167_SOURCE,SPEC167_PORT,SPEC167_MODE \
  "$SOURCE/specs/167-itiger-repo-throughput/jobs/rank.sh" \
  > "$PARTIAL/rank-step.log" 2> "$PARTIAL/rank-step.err"
test -f "$PARTIAL/rank-complete-0" -a -f "$PARTIAL/rank-complete-1"
test -f "$PARTIAL/preflight/preflight.json"
test -f "$PARTIAL/preflight/preflight.cold.json"
test -z "$(find "$PARTIAL" -type f \( -name '*.privkey' -o -name 'bootstrap-tokens.txt' -o -name 'payload.bin' -o -name '*.sqlite3' \) -print -quit)"
state=PASS
