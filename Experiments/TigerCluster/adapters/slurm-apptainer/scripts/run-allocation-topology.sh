#!/bin/bash
set -euo pipefail

usage() {
  echo "usage: $0 --process-map FILE --scratch DIR --evidence DIR --nfd-template FILE" >&2
  exit 2
}
process_map= scratch= evidence= nfd_template=
while (($#)); do
  case "$1" in
    --process-map) process_map=$2; shift 2 ;;
    --scratch) scratch=$2; shift 2 ;;
    --evidence) evidence=$2; shift 2 ;;
    --nfd-template) nfd_template=$2; shift 2 ;;
    *) usage ;;
  esac
done
for value in "$process_map" "$scratch" "$evidence" "$nfd_template"; do [[ -n $value ]] || usage; done
[[ -f $process_map && -f $nfd_template ]] || usage
[[ -n ${SLURM_JOB_ID:-} || ${NDNSF_SPEC110_TEST_MODE:-0} == 1 ]] || {
  echo SPEC110_TOPOLOGY_REQUIRES_ALLOCATION >&2; exit 3;
}
case "$scratch" in /tmp/ndnsf-di-*) ;; *) echo SPEC110_TOPOLOGY_SCRATCH_INVALID >&2; exit 3 ;; esac

container_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
lib="$container_root/lib"
route_config="$container_root/adapters/slurm-apptainer/scripts/configure-allocation-routes.sh"
mkdir -p "$scratch/log" "$scratch/readiness" "$evidence/processes" "$evidence/generated"
chmod 700 "$scratch"

PYTHONPATH="$lib" python3 - "$process_map" "$nfd_template" "$scratch" "$evidence" <<'PY'
import json,shlex,sys
from pathlib import Path
from allocation_topology import load_process_map,render_nfd_config
value=load_process_map(sys.argv[1]);template=Path(sys.argv[2]).read_text();scratch=Path(sys.argv[3]);evidence=Path(sys.argv[4]);generated=evidence/'generated'
(evidence/'frozen-process-map.json').write_text(json.dumps(value,indent=2,sort_keys=True)+'\n')
for node in value['nodes']:
 state=scratch/'nfd'/str(node['nodeRank']);state.mkdir(parents=True,exist_ok=True)
 config=generated/f"nfd-{node['nodeRank']}.conf"
 config.write_text(render_nfd_config(template,node,str(state)))
for process in value['processes']:
 script=generated/(process['processId']+'.sh')
 environment=f"export NDN_CLIENT_TRANSPORT={shlex.quote('unix://'+process['nfdSocket'])}\n"
 script.write_text('#!/bin/bash\nset -euo pipefail\n'+environment+'exec '+shlex.join(process['command'])+'\n')
 script.chmod(0o700)
PY

step_pids=()
cleanup() {
  rc=$?
  trap - EXIT INT TERM
  for pid in "${step_pids[@]:-}"; do kill -- "-$pid" 2>/dev/null || kill "$pid" 2>/dev/null || true; done
  for pid in "${step_pids[@]:-}"; do wait "$pid" 2>/dev/null || true; done
  survivors=0
  if command -v srun >/dev/null 2>&1; then
    srun --overlap --nodes="${SLURM_NNODES:-1}" --ntasks="${SLURM_NNODES:-1}" --ntasks-per-node=1 \
      "--export=ALL,SPEC110_SCRATCH_AUDIT=$scratch,SPEC110_AUDIT_IGNORE_PID=$$" sh -c \
      'ps -eo pid=,args= | awk '\''{pid=$1; $1=""} pid != ENVIRON["SPEC110_AUDIT_IGNORE_PID"] && index($0,ENVIRON["SPEC110_SCRATCH_AUDIT"]) {print pid $0; found=1} END {exit found ? 0 : 1}'\''' \
      >"$evidence/survivors.txt" 2>&1 && survivors=1 || true
  fi
  printf '{"slurmJobId":"%s","exitCode":%d,"survivors":%d,"status":"%s"}\n' \
    "${SLURM_JOB_ID:-test}" "$rc" "$survivors" "$([[ $rc -eq 0 && $survivors -eq 0 ]] && echo PASS || echo FAIL)" \
    >"$evidence/teardown.json"
  [[ $survivors -eq 0 ]] || rc=9
  exit "$rc"
}
signal_exit() {
  case "$1" in TERM) exit 143 ;; INT) exit 130 ;; esac
}
trap cleanup EXIT
trap 'signal_exit TERM' TERM
trap 'signal_exit INT' INT

mapfile -t node_rows < <(PYTHONPATH="$lib" python3 - "$process_map" <<'PY'
import sys
from allocation_topology import load_process_map
value=load_process_map(sys.argv[1]);nodes={n['nodeRank']:n for n in value['nodes']}
for process in value['processes']:
 if process['kind']=='nfd':
  command=process['command'];index=command.index('--config')
  print(process['nodeRank'],process['nfdSocket'],command[index+1],sep='\t')
PY
)
for row in "${node_rows[@]}"; do
  IFS=$'\t' read -r rank socket config <<<"$row"
  srun --exclusive --nodes=1 --ntasks=1 "--relative=$rank" mkdir -p "$(dirname "$socket")" "$(dirname "$config")"
  srun --exclusive --nodes=1 --ntasks=1 "--relative=$rank" tee "$config" \
    <"$evidence/generated/nfd-$rank.conf" >/dev/null
  setsid srun --exclusive --nodes=1 --ntasks=1 "--relative=$rank" \
    "$evidence/generated/nfd-$rank.sh" >"$scratch/log/nfd-$rank.log" 2>&1 &
  step_pids+=("$!")
done

deadline=$((SECONDS+30))
for row in "${node_rows[@]}"; do
  IFS=$'\t' read -r rank socket config <<<"$row"
  ready=0
  while ((SECONDS < deadline)); do
    if srun --overlap --nodes=1 --ntasks=1 "--relative=$rank" test -S "$socket"; then ready=1; break; fi
    sleep 0.2
  done
  [[ $ready -eq 1 ]] || { echo "SPEC110_NFD_READINESS_TIMEOUT:$rank" >&2; exit 5; }
  printf '%s\n' "$SECONDS" >"$scratch/readiness/nfd-$rank-ready"
done

placement=$(PYTHONPATH="$lib" python3 -c 'import sys;from allocation_topology import load_process_map;print(load_process_map(sys.argv[1])["placementClass"])' "$process_map")
if [[ $placement == multi-node ]]; then
  "$route_config" --process-map "$process_map" --evidence "$evidence/routes"
fi

launch_kind() {
  local kind=$1 foreground=${2:-0}
  while IFS=$'\t' read -r process_id rank gpu_rank; do
    [[ -n $process_id ]] || continue
    command=(srun --exclusive --nodes=1 --ntasks=1 "--relative=$rank")
    [[ $gpu_rank == null ]] || command+=(--gpus-per-task=1 "--gpu-bind=map_gpu:$gpu_rank")
    command+=("$evidence/generated/$process_id.sh")
    if [[ $foreground == 1 ]]; then
      "${command[@]}" >"$scratch/log/$process_id.log" 2>&1
    else
      setsid "${command[@]}" >"$scratch/log/$process_id.log" 2>&1 & step_pids+=("$!")
    fi
  done < <(PYTHONPATH="$lib" python3 - "$process_map" "$kind" <<'PY'
import sys
from allocation_topology import load_process_map
for p in load_process_map(sys.argv[1])['processes']:
 if p['kind']==sys.argv[2]: print(p['processId'],p['nodeRank'],'null' if p['gpuRank'] is None else p['gpuRank'],sep='\t')
PY
)
}

launch_kind controller
sleep "${NDNSF_SPEC110_READINESS_SETTLE_SECONDS:-1}"
grep -Eq 'controller-ready|NDNSF_DI_CONTROLLER_READY|ServiceController started' "$scratch/log/controller.log" || {
  echo SPEC110_CONTROLLER_READINESS_TIMEOUT >&2; exit 7;
}
launch_kind provider
sleep "${NDNSF_SPEC110_READINESS_SETTLE_SECONDS:-1}"
for pid in "${step_pids[@]}"; do kill -0 "$pid" 2>/dev/null || { echo SPEC110_PARTIAL_READINESS >&2; exit 7; }; done
for log in "$scratch"/log/provider-*.log; do
  grep -Eq 'provider-[0-9]+-ready|NDNSF_DI_NATIVE_PROVIDER_READY' "$log" || {
    echo "SPEC110_PROVIDER_READINESS_TIMEOUT:$log" >&2; exit 7;
  }
done
launch_kind user 1

cp -a "$scratch/log/." "$evidence/processes/"
printf 'CANDIDATE_PROCESS_GRAPH_COMPLETED\n' >"$evidence/readiness-verdict.txt"
