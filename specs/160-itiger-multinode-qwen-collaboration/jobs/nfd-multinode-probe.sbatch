#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=3
#SBATCH --ntasks=3
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=8G
#SBATCH --gres=gpu:rtx_5000:1
#SBATCH --time=00:10:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec160/probe/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec160/probe/slurm-%j.err

set -Eeuo pipefail
umask 077
: "${SPEC160_RUN_ID:?}"
: "${SPEC160_SUBMISSION_ID:?}"
: "${SPEC160_REPLACES_JOB:?}"
: "${SPEC160_SOURCE_ROOT:?}"
: "${SPEC160_SOURCE_SHA256:?}"
: "${SPEC160_SIF:?}"
: "${SPEC160_SIF_SHA256:?}"

readonly evidence_parent='/project/tma1/ndnsf-di/evidence/spec160/probe'
readonly partial="${evidence_parent}/.${SPEC160_SUBMISSION_ID}.partial"
readonly final="${evidence_parent}/${SPEC160_SUBMISSION_ID}"
test ! -e "$partial" && test ! -e "$final"
mkdir -p "$partial/source"
cp "$SPEC160_SOURCE_ROOT"/nfd.conf.in \
   "$SPEC160_SOURCE_ROOT"/nfd-probe-rank.sh \
   "$SPEC160_SOURCE_ROOT"/nfd-probe-rank-inner.sh \
   "$SPEC160_SOURCE_ROOT"/raw-udp-probe.py \
   "$SPEC160_SOURCE_ROOT"/ndn-data-probe \
   "$SPEC160_SOURCE_ROOT"/ndn-data-probe.cpp \
   "$SPEC160_SOURCE_ROOT"/nfd-multinode-probe.sbatch \
   "$partial/source/"
chmod 700 "$partial/source/nfd-probe-rank.sh" \
          "$partial/source/nfd-probe-rank-inner.sh" \
          "$partial/source/raw-udp-probe.py" \
          "$partial/source/ndn-data-probe"
printf '%s  source-bundle\n' "$SPEC160_SOURCE_SHA256" > "$partial/source-bundle.sha256"
printf '%s  %s\n' "$SPEC160_SIF_SHA256" "$SPEC160_SIF" | sha256sum -c - \
  > "$partial/sif-sha256.log"
export SPEC160_SHARED="$partial"

terminal_state=FAIL
finish()
{
  rc=$?
  trap - EXIT INT TERM
  python3 - "$partial/result.json" "$terminal_state" "$rc" <<'PY' || true
import json,os,sys
path,state,rc=sys.argv[1:]
with open(path,"w",encoding="utf-8") as f:
  json.dump({"schemaVersion":"spec160-nfd-probe-v1","state":state,
             "exitCode":int(rc),"jobId":os.environ["SLURM_JOB_ID"],
             "runId":os.environ["SPEC160_RUN_ID"],
             "submissionId":os.environ["SPEC160_SUBMISSION_ID"],
             "replacesFailedJob":os.environ["SPEC160_REPLACES_JOB"]},
            f,indent=2,sort_keys=True)
  f.write("\n")
PY
  find "$partial" -type f -print0 | sort -z | xargs -0 sha256sum \
    > "$partial/checksums.sha256" 2>/dev/null || true
  if test "$terminal_state" = PASS && test "$rc" -eq 0; then
    mv "$partial" "$final"
  fi
  exit "$rc"
}
trap finish EXIT INT TERM

comment=$(scontrol show job "$SLURM_JOB_ID" -o | tr ' ' '\n' | sed -n 's/^Comment=//p')
test "$comment" = "spec160:${SPEC160_SUBMISSION_ID}"
mapfile -t nodes < <(scontrol show hostnames "$SLURM_JOB_NODELIST")
test "${#nodes[@]}" -eq 3
test "$(printf '%s\n' "${nodes[@]}" | sort -u | wc -l)" -eq 3
printf '%s\n' "${nodes[@]}" > "$partial/nodes.txt"
port=$((20000 + SLURM_JOB_ID % 20000))
printf '%s\n' "$port" > "$partial/port.txt"
export SPEC160_PORT="$port"
srun --kill-on-bad-exit=1 --label \
  --export=ALL,SPEC160_SHARED="$partial",SPEC160_SIF="$SPEC160_SIF",SPEC160_PORT="$port" \
  "$partial/source/nfd-probe-rank.sh" \
  > "$partial/rank-step.log" 2> "$partial/rank-step.err"
test -f "$partial/rank-complete-0"
test -f "$partial/rank-complete-1"
test -f "$partial/rank-complete-2"

python3 - "$partial" <<'PY'
import csv,json,sys
from pathlib import Path
root=Path(sys.argv[1])
nodes=[]
for rank in range(3):
  host=(root/f"node-{rank}/hostname.txt").read_text().strip()
  ip=(root/f"node-{rank}/ipv4.txt").read_text().strip()
  gpu=next(csv.reader((root/f"node-{rank}/gpu.csv").open()))[0].strip()
  nodes.append({"rank":rank,"hostname":host,"ipv4":ip,"gpuUuid":gpu})
assert len({x["hostname"] for x in nodes}) == 3
assert len({x["gpuUuid"] for x in nodes}) == 3
doc={"schemaVersion":"spec160-nfd-probe-analysis-v1","status":"PASS",
     "nodes":nodes,"paths":[]}
for transport in ("tcp","udp"):
  text=(root/f"{transport}-consumer.log").read_text()
  assert "payloadMatch=true" in text
  doc["paths"].append({"transport":transport,"producerRank":0,
                       "relayRank":1,"consumerRank":2,"consumerLog":text.strip()})
(root/"analysis.json").write_text(json.dumps(doc,indent=2,sort_keys=True)+"\n")
PY
terminal_state=PASS
