#!/bin/bash
set -euo pipefail

usage() {
  echo "usage: $0 --process-map FILE --scratch DIR --evidence DIR --nfd-template FILE --workdir DIR" >&2
  exit 2
}
process_map= scratch= evidence= nfd_template= workdir=
while (($#)); do
  case "$1" in
    --process-map) process_map=$2; shift 2 ;;
    --scratch) scratch=$2; shift 2 ;;
    --evidence) evidence=$2; shift 2 ;;
    --nfd-template) nfd_template=$2; shift 2 ;;
    --workdir) workdir=$2; shift 2 ;;
    *) usage ;;
  esac
done
for value in "$process_map" "$scratch" "$evidence" "$nfd_template" "$workdir"; do [[ -n $value ]] || usage; done
[[ -f $process_map && -f $nfd_template ]] || usage

# Install the pre-start evidence trap before validating allocation-specific
# paths. Invalid workdir/scratch or allocation mode is still an entered
# topology attempt and must retain its original failure boundary.
mkdir -p "$evidence"
prestart_cleanup() {
  rc=$?
  trap - EXIT INT TERM
  printf '{"slurmJobId":"%s","exitCode":%d,"survivors":0,"status":"FAIL"}\n' \
    "${SLURM_JOB_ID:-test}" "$rc" >"$evidence/teardown.json"
  exit "$rc"
}
prestart_signal_exit() {
  case "$1" in TERM) exit 143 ;; INT) exit 130 ;; esac
}
trap prestart_cleanup EXIT
trap 'prestart_signal_exit TERM' TERM
trap 'prestart_signal_exit INT' INT

[[ -d $workdir && $workdir = /* ]] || { echo SPEC110_WORKDIR_INVALID >&2; exit 3; }
[[ -n ${SLURM_JOB_ID:-} || ${NDNSF_SPEC110_TEST_MODE:-0} == 1 ]] || {
  echo SPEC110_TOPOLOGY_REQUIRES_ALLOCATION >&2; exit 3;
}
case "$scratch" in /tmp/ndnsf-di-*) ;; *) echo SPEC110_TOPOLOGY_SCRATCH_INVALID >&2; exit 3 ;; esac
if [[ ${NDNSF_SPEC110_TEST_MODE:-0} != 1 && -n ${SLURM_JOB_ID:-} ]]; then
  scratch_name=${scratch##*/}
  case "$scratch_name" in
    "ndnsf-di-${SLURM_JOB_ID}"|"ndnsf-di-${SLURM_JOB_ID}-"*) ;;
    *) echo "SPEC110_TOPOLOGY_SCRATCH_JOB_MISMATCH:$scratch" >&2; exit 3 ;;
  esac
fi

container_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
lib="$container_root/lib"
route_config="$container_root/adapters/slurm-apptainer/scripts/configure-allocation-routes.sh"
mkdir -p "$scratch/log" "$scratch/readiness" "$evidence/processes" "$evidence/generated"
chmod 700 "$scratch"

# Long-lived topology processes share an allocation node (NFD, Controller and
# one or more Providers).  A whole-node exclusive step reserves every CPU/GRES
# on the target node for one step and can therefore serialize or deadlock later
# siblings.  Request one exact CPU per step and explicitly allow overlap;
# Provider GPU ownership is still enforced by --gpus-per-task/--gpu-bind below.
srun_step=(srun --overlap --exact --nodes=1 --ntasks=1 --cpus-per-task=1)
srun_node() {
  local rank=$1
  shift
  "${srun_step[@]}" "--relative=$rank" "$@"
}

PYTHONPATH="$lib" python3 - "$process_map" "$nfd_template" "$scratch" "$evidence" "$workdir" <<'PY'
import json,sys
from pathlib import Path
from allocation_topology import load_process_map,render_nfd_config,render_process_launcher
value=load_process_map(sys.argv[1]);template=Path(sys.argv[2]).read_text();scratch=Path(sys.argv[3]);evidence=Path(sys.argv[4]);workdir=Path(sys.argv[5]);generated=evidence/'generated'
(evidence/'frozen-process-map.json').write_text(json.dumps(value,indent=2,sort_keys=True)+'\n')
for node in value['nodes']:
 state=scratch/'nfd'/str(node['nodeRank']);state.mkdir(parents=True,exist_ok=True)
 config=generated/f"nfd-{node['nodeRank']}.conf"
 config.write_text(render_nfd_config(template,node,str(state)))
for process in value['processes']:
 script=generated/(process['processId']+'.sh')
 script.write_text(render_process_launcher(process,scratch,workdir))
 script.chmod(0o700)
PY

mapfile -t workdir_ranks < <(PYTHONPATH="$lib" python3 - "$process_map" <<'PY'
import sys
from allocation_topology import load_process_map
for node in load_process_map(sys.argv[1])['nodes']:
 print(node['nodeRank'])
PY
)
for rank in "${workdir_ranks[@]}"; do
  # The application bundle may be on submit-host storage. Check visibility on
  # every target node before starting even one NFD, so a partial startup cannot
  # masquerade as a later application or protocol failure.
  srun_node "$rank" test -d "$workdir" || {
    echo "SPEC110_WORKDIR_NOT_VISIBLE:$rank" >&2
    exit 4
  }
done

mapfile -t process_rows < <(PYTHONPATH="$lib" python3 - "$process_map" <<'PY'
import sys
from allocation_topology import load_process_map
for process in load_process_map(sys.argv[1])['processes']:
 print(process['processId'],process['nodeRank'],sep='\t')
PY
)
for row in "${process_rows[@]}"; do
  IFS=$'\t' read -r process_id rank <<<"$row"
  # Evidence may be on submit-host/shared storage that is not mounted on every
  # compute node. Materialize each generated launcher on its target node's
  # job-local scratch before any srun step tries to execute it.
  srun_node "$rank" mkdir -p "$scratch/generated"
  srun_node "$rank" tee \
    "$scratch/generated/$process_id.sh" <"$evidence/generated/$process_id.sh" >/dev/null
  srun_node "$rank" chmod 700 \
    "$scratch/generated/$process_id.sh"
done

# Identity sources live on the execution nodes, not on the submit host.  Check
# the complete read-only PIB/TPM input set before starting any NFD, so an
# unmounted identity directory cannot leave a partially started topology.
if [[ ${NDNSF_SPEC110_TEST_MODE:-0} != 1 ]]; then
  mapfile -t identity_rows < <(PYTHONPATH="$lib" python3 - "$process_map" <<'PY'
import sys
from allocation_topology import load_process_map
for process in load_process_map(sys.argv[1])['processes']:
 if process['kind'] != 'nfd':
  print(process['nodeRank'],process['identityRef'],sep='\t')
PY
  )
  for row in "${identity_rows[@]}"; do
    IFS=$'\t' read -r rank identity <<<"$row"
    srun_node "$rank" test -r "$identity/.ndn/pib.db" || {
      echo "SPEC110_IDENTITY_NOT_VISIBLE:$rank:$identity" >&2
      exit 4
    }
    srun_node "$rank" test -r "$identity/.ndn/ndnsec-key-file" || {
      echo "SPEC110_IDENTITY_NOT_VISIBLE:$rank:$identity" >&2
      exit 4
    }
    srun_node "$rank" test ! -L "$identity" || {
      echo "SPEC110_IDENTITY_SYMLINK_FORBIDDEN:$rank:$identity" >&2
      exit 4
    }
    srun_node "$rank" sh -c 'test -z "$(find "$1" -type l -print -quit)"' sh "$identity/.ndn" || {
      echo "SPEC110_IDENTITY_SYMLINK_FORBIDDEN:$rank:$identity" >&2
      exit 4
    }
  done
fi

# A syntactically valid address can still belong to a different interface or
# host. Bind the declared address on its target node before starting any NFD;
# MiniNDN's loopback topology would otherwise hide a bad multi-node map.
if [[ ${NDNSF_SPEC110_TEST_MODE:-0} != 1 ]]; then
  mapfile -t address_rows < <(PYTHONPATH="$lib" python3 - "$process_map" <<'PY'
import sys
from allocation_topology import load_process_map
for node in load_process_map(sys.argv[1])['nodes']:
 print(node['nodeRank'],node['address'],sep='\t')
PY
  )
  address_probe='import socket,sys
sock=socket.socket(socket.AF_INET,socket.SOCK_STREAM)
try:
    sock.bind((sys.argv[1],0))
finally:
    sock.close()'
  for row in "${address_rows[@]}"; do
    IFS=$'\t' read -r rank address <<<"$row"
    srun_node "$rank" python3 -c "$address_probe" "$address" || {
      echo "SPEC110_NODE_ADDRESS_NOT_LOCAL:$rank:$address" >&2
      exit 4
    }
  done
fi

# Detect a port already occupied by another job on the target node before NFD
# startup.  Slurm allocations may overlap on a node, so a map-level range check
# alone cannot catch a concurrent listener.  This is a bounded preflight; NFD
# still remains the final authority if a race occurs after the probe.
mapfile -t port_rows < <(PYTHONPATH="$lib" python3 - "$process_map" <<'PY'
import sys
from allocation_topology import load_process_map
for node in load_process_map(sys.argv[1])['nodes']:
 print(node['nodeRank'],node['tcpPort'],node['udpPort'],sep='\t')
PY
)
for row in "${port_rows[@]}"; do
  IFS=$'\t' read -r rank tcp_port udp_port <<<"$row"
  port_probe='import socket,sys
for kind,port in ((socket.SOCK_STREAM,int(sys.argv[1])),(socket.SOCK_DGRAM,int(sys.argv[2]))):
    sock=socket.socket(socket.AF_INET,kind)
    try:
        sock.bind(("0.0.0.0",port))
    finally:
        sock.close()'
  srun_node "$rank" python3 -c "$port_probe" "$tcp_port" "$udp_port" || {
    echo "SPEC110_PORT_NOT_AVAILABLE:$rank:$tcp_port:$udp_port" >&2
    exit 4
  }
done

trap - EXIT INT TERM
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
value=load_process_map(sys.argv[1])
for process in value['processes']:
 if process['kind']=='nfd':
  print(process['nodeRank'],process['nfdSocket'],sep='\t')
PY
)
declare -A nfd_steps=()
for row in "${node_rows[@]}"; do
  IFS=$'\t' read -r rank socket <<<"$row"
  config="$scratch/generated/nfd-$rank.conf"
  srun_node "$rank" mkdir -p "$(dirname "$socket")" "$(dirname "$config")"
  # A reused job scratch can contain a stale socket left by an earlier NFD
  # crash. Remove it on the target node before launching this instance.
  srun_node "$rank" rm -f "$socket"
  srun_node "$rank" tee "$config" \
    <"$evidence/generated/nfd-$rank.conf" >/dev/null
  setsid "${srun_step[@]}" "--relative=$rank" \
    "$scratch/generated/nfd-$rank.sh" >"$scratch/log/nfd-$rank.log" 2>&1 &
  nfd_steps["$rank"]=$!
  step_pids+=("${nfd_steps[$rank]}")
done

for row in "${node_rows[@]}"; do
  IFS=$'\t' read -r rank socket <<<"$row"
  # Give each node its own bounded readiness window.  A slow first node must
  # not consume the entire budget for later nodes in a multi-node allocation.
  deadline=$((SECONDS+30))
  ready=0
  while ((SECONDS < deadline)); do
    if kill -0 "${nfd_steps[$rank]}" 2>/dev/null && srun_node "$rank" test -S "$socket"; then
      ready=1; break
    fi
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
    command=("${srun_step[@]}" "--relative=$rank")
    [[ $gpu_rank == null ]] || command+=(--gpus-per-task=1 "--gpu-bind=map_gpu:$gpu_rank")
    command+=("$scratch/generated/$process_id.sh")
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
