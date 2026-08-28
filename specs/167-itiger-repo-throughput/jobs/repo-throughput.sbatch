#!/bin/bash
#SBATCH --job-name=spec167-repo-throughput
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=2
#SBATCH --nodelist=itiger07,itiger08
#SBATCH --ntasks=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=16G
#SBATCH --time=02:30:00
#SBATCH --output=/project/tma1/ndnsf-di/slurm/spec167-throughput-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/slurm/spec167-throughput-%j.err

set -Eeuo pipefail
umask 077
: "${SPEC167_SUBMISSION_ID:?}" "${SPEC167_CAMPAIGN_ID:?}" "${SPEC167_SOURCE_ROOT:?}"
readonly ROOT=/project/tma1/ndnsf-di
readonly SIF="$ROOT/releases/spec166-dcef2858c060/runtime.sif"
readonly SOURCE="$SPEC167_SOURCE_ROOT"
readonly PARENT="$ROOT/evidence/spec167/campaign"
readonly PARTIAL="$PARENT/.${SPEC167_SUBMISSION_ID}.partial"
readonly FINAL="$PARENT/${SPEC167_SUBMISSION_ID}"
comment=$(scontrol show job "$SLURM_JOB_ID" -o | tr ' ' '\n' | sed -n 's/^Comment=//p')
test "$comment" = "spec167:${SPEC167_SUBMISSION_ID}"
test ! -e "$PARTIAL" && test ! -e "$FINAL"
mkdir -p "$PARTIAL/runs" "$ROOT/slurm"
state=FAIL
finish()
{
  rc=$?; trap - EXIT INT TERM
  python3 - "$PARTIAL/result-terminal.json" "$state" "$rc" "$SLURM_JOB_ID" "$SPEC167_SUBMISSION_ID" <<'PY' || true
import json,sys
p,state,rc,job,submission=sys.argv[1:]
json.dump({'schemaVersion':1,'state':state,'exitCode':int(rc),'jobId':job,'submissionId':submission},open(p,'w'),indent=2,sort_keys=True); open(p,'a').write('\n')
PY
  (cd "$PARTIAL" && find . -type f ! -name checksums.sha256 -print0 | sort -z | xargs -0 sha256sum) > "$PARTIAL/checksums.sha256" || true
  if test "$state" = PASS && test "$rc" -eq 0; then mv "$PARTIAL" "$FINAL"; fi
  exit "$rc"
}
trap finish EXIT INT TERM
(cd "$SOURCE" && sha256sum -c source-checksums.sha256) > "$PARTIAL/source-checksums.log"
sif_sha=$(jq -r .sifSha256 "$ROOT/releases/spec166-dcef2858c060/materialization.json"); sif_sha=${sif_sha#sha256:}
printf '%s  %s\n' "$sif_sha" "$SIF" | sha256sum -c - > "$PARTIAL/sif-checksum.log"
mapfile -t nodes < <(scontrol show hostnames "$SLURM_JOB_NODELIST")
printf '%s\n' "${nodes[@]}" > "$PARTIAL/nodes.txt"; test "${nodes[*]}" = "itiger07 itiger08"
python3 "$SOURCE/Experiments/NDNSF_DistributedRepo_Artifact_Itiger.py" \
  --role manifest --campaign-id "$SPEC167_CAMPAIGN_ID" \
  --output "$PARTIAL/campaign-manifest.json" > "$PARTIAL/manifest-sha256.txt"
base_port=$((22000 + SLURM_JOB_ID % 10000))

python3 - "$PARTIAL/campaign-manifest.json" <<'PY' > "$PARTIAL/schedule.tsv"
import json,sys
for r in json.load(open(sys.argv[1]))['schedule']:
 print(r['runId'],r['subject'],r['sizeBytes'],r['repetition'],str(r['warmup']).lower(),sep='\t')
PY

run_index=0
while IFS=$'\t' read -r run_id subject size repetition warmup; do
  printf '{"event":"run-start","runId":"%s","time":%s}\n' "$run_id" "$(date +%s)" >> "$PARTIAL/progress.jsonl"
  sub_name="${run_id}-sub0"; run_status=PASS
  export SPEC167_SHARED="$PARTIAL" SPEC167_SIF="$SIF" SPEC167_SOURCE="$SOURCE"
  run_port=$((base_port + run_index * 32))
  export SPEC167_PORT="$run_port" SPEC167_TCP_PORT=$((run_port+1))
  export SPEC167_MODE=formal SPEC167_RUN_NAME="$sub_name"
  export SPEC167_SUBJECT="$subject" SPEC167_PAYLOAD_SIZE="$size"
  export SPEC167_DURATION_SECONDS=60
  mkdir -p "$PARTIAL/runs/${sub_name}"
  if ! srun --kill-on-bad-exit=1 --label \
    --export=ALL,SPEC167_SHARED,SPEC167_SIF,SPEC167_SOURCE,SPEC167_PORT,SPEC167_TCP_PORT,SPEC167_MODE,SPEC167_RUN_NAME,SPEC167_SUBJECT,SPEC167_PAYLOAD_SIZE,SPEC167_DURATION_SECONDS \
    "$SOURCE/specs/167-itiger-repo-throughput/jobs/rank.sh" < /dev/null \
    > "$PARTIAL/runs/${sub_name}/rank-step.log" 2> "$PARTIAL/runs/${sub_name}/rank-step.err"; then
    run_status=FAIL
  else
    if test "$subject" = physical-network; then result="$PARTIAL/runs/${sub_name}/tcp-result.json"; else result="$PARTIAL/runs/${sub_name}/result.json"; fi
    if ! python3 -c 'import json,sys; raise SystemExit(0 if float(json.load(open(sys.argv[1]))["elapsedMs"]) >= 60000 else 1)' "$result"; then
      run_status=FAIL
    fi
  fi
  python3 - "$PARTIAL" "$run_id" "$subject" "$size" "$repetition" "$warmup" "$run_status" <<'PY'
import glob,json,os,sys
root,run_id,subject,size,rep,warmup,status=sys.argv[1:]
paths=sorted(glob.glob(f'{root}/runs/{run_id}-sub*'))
pub=[]; cold=[]; warm=[]
for p in paths:
 f=os.path.join(p,'tcp-result.json' if subject=='physical-network' else 'result.json')
 if os.path.isfile(f): pub.append(json.load(open(f)))
 c=os.path.join(p,'result.cold.json')
 if os.path.isfile(c): cold.append(json.load(open(c)))
 w=os.path.join(p,'result.warm.json')
 if os.path.isfile(w): warm.append(json.load(open(w)))
elapsed=sum(float(x.get('elapsedMs',0)) for x in pub); logical=sum(int(x.get('logicalBytes',0)) for x in pub)
row={'schemaVersion':1,'runId':run_id,'pairId':f's{size}-rep{rep}','subject':subject,'sizeBytes':int(size),'repetition':int(rep),'warmup':warmup=='true','status':status if pub else 'FAIL','subtransfers':sum(int(x.get('subtransfers',1)) for x in pub),'elapsedMs':elapsed,'logicalBytes':logical,'logicalGoodputMbps':logical*8/elapsed/1000 if elapsed else 0.0,'dataPath':f'/scratch/data/{run_id}-sub*','storePath':f'/scratch/data/{run_id}-sub*/stores','destinationPath':f'/scratch/data/{run_id}-sub*/cold-destinations','coldLogicalBytes':sum(int(x.get('logicalBytes',0)) for x in cold),'coldElapsedMs':sum(float(x.get('elapsedMs',0)) for x in cold),'timeoutCount':sum(int(x.get('timeoutCount',0)) for x in pub+cold),'retransmissionCount':sum(int(x.get('retransmissionCount',0)) for x in pub+cold),'warmReuseCount':sum(int(x.get('subtransfers',1)) for x in warm),'duplicatePayloadBytesWritten':sum(int(x.get('duplicatePayloadBytesWritten',0)) for x in warm)}
with open(os.path.join(root,'run-records.jsonl'),'a') as out: out.write(json.dumps(row,sort_keys=True,separators=(',',':'))+'\n')
PY
  printf '{"event":"run-end","runId":"%s","status":"%s","time":%s}\n' "$run_id" "$run_status" "$(date +%s)" >> "$PARTIAL/progress.jsonl"
  run_index=$((run_index+1))
done < "$PARTIAL/schedule.tsv"

python3 "$SOURCE/Experiments/analyze_spec167_itiger_repo.py" \
  --manifest "$PARTIAL/campaign-manifest.json" --runs "$PARTIAL/run-records.jsonl" \
  --output "$PARTIAL/derived-results.json" > "$PARTIAL/analysis.log"
grep -Fq SPEC167_ANALYSIS_PASS "$PARTIAL/analysis.log"
test -z "$(find "$PARTIAL" -type f \( -name '*.privkey' -o -name 'bootstrap-tokens.txt' -o -name 'payload.bin' -o -name '*.sqlite3' \) -print -quit)"
state=PASS
