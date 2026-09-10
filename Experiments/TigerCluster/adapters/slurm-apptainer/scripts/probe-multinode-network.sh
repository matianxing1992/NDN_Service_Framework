#!/bin/bash
set -euo pipefail

usage() {
  echo "usage: $0 --process-map FILE --output FILE" >&2
  exit 2
}

process_map= output=
while (($#)); do
  case "$1" in
    --process-map) process_map=$2; shift 2 ;;
    --output) output=$2; shift 2 ;;
    *) usage ;;
  esac
done
[[ -n $process_map && -n $output && -f $process_map ]] || usage
[[ -n ${SLURM_JOB_ID:-} || ${NDNSF_SPEC110_TEST_MODE:-0} == 1 ]] || {
  echo SPEC110_NETWORK_PROBE_REQUIRES_ALLOCATION >&2; exit 3;
}

container_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
lib="$container_root/lib"
mkdir -p "$(dirname "$output")"

if [[ -n ${NDNSF_SPEC110_PROBE_OBSERVATION:-} ]]; then
  PYTHONPATH="$lib" python3 - "$process_map" "$NDNSF_SPEC110_PROBE_OBSERVATION" "$output" <<'PY'
import json,sys
from pathlib import Path
from allocation_topology import evaluate_transport_probe,load_process_map
process_map=load_process_map(sys.argv[1]);observations=json.loads(Path(sys.argv[2]).read_text())
result=evaluate_transport_probe(process_map,observations)
Path(sys.argv[3]).write_text(json.dumps({"processMap":process_map,"observations":observations,"verdict":result},indent=2,sort_keys=True)+"\n")
PY
  exit 0
fi

# The live diagnostic lane also invokes ``srun --relative=<fromNodeRank>``.
# Bind that rank to Slurm's canonical hostname order before probing any route;
# otherwise a diagnostic result could be collected from the wrong node while
# the supervisor and route entry point use a different mapping.
if [[ ${NDNSF_SPEC110_TEST_MODE:-0} != 1 ]]; then
  [[ -n ${SLURM_JOB_NODELIST:-} ]] || {
    echo SPEC110_ALLOCATION_NODELIST_MISSING >&2
    exit 4
  }
  command -v scontrol >/dev/null 2>&1 || {
    echo SPEC110_ALLOCATION_NODELIST_UNAVAILABLE >&2
    exit 4
  }
  if ! allocation_nodes_output=$(scontrol show hostnames "$SLURM_JOB_NODELIST"); then
    echo SPEC110_ALLOCATION_NODELIST_QUERY_FAILED >&2
    exit 4
  fi
  [[ -n $allocation_nodes_output ]] || {
    echo SPEC110_ALLOCATION_NODELIST_EMPTY >&2
    exit 4
  }
  mapfile -t allocation_nodes <<<"$allocation_nodes_output"
  if ! PYTHONPATH="$lib" python3 - "$process_map" "${allocation_nodes[@]}" <<'PY'
import sys
from allocation_topology import load_process_map, validate_allocation_node_order

try:
    validate_allocation_node_order(load_process_map(sys.argv[1]), sys.argv[2:])
except Exception as exc:
    print(exc, file=sys.stderr)
    raise SystemExit(4)
PY
  then
    echo SPEC110_ALLOCATION_NODE_ORDER_MISMATCH >&2
    exit 4
  fi
fi

PYTHONPATH="$lib" python3 - "$process_map" "$output" <<'PY'
import json,os,subprocess,sys
from pathlib import Path
from allocation_topology import TopologyError,evaluate_transport_probe,load_process_map

process_map=load_process_map(sys.argv[1])
addresses=[node["address"] for node in process_map["nodes"]]
nodes={node["nodeRank"]: node for node in process_map["nodes"]}
observations={"allocationAddresses":addresses}
for transport in ("tcp","udp"):
    closed=[];reachable=0
    for route in process_map["routes"]:
        # The map stores routes for the selected transport, but the
        # diagnostic lane must probe the corresponding port for *its own*
        # transport. Reusing route["port"] would test UDP on a TCP port (or
        # vice versa) and produce a misleading diagnostic result.
        target = nodes[route["toNodeRank"]]
        address = target["address"]
        port = target[transport + "Port"]
        # Use the in-image Python socket module instead of assuming that an
        # optional netcat package is installed on every compute node.  TCP
        # connect and UDP connect preserve the old bounded diagnostic probe;
        # neither lane is a protocol qualification result.
        probe = (
            "import socket,sys; "
            "kind=socket.SOCK_DGRAM if sys.argv[3]=='udp' else socket.SOCK_STREAM; "
            "sock=socket.socket(socket.AF_INET,kind); sock.settimeout(2); "
            "sock.connect((sys.argv[1],int(sys.argv[2]))); sock.close()"
        )
        # The probe runs inside an allocation whose NFD may already consume a
        # step on the same node.  Do not request an exclusive whole-node step;
        # one exact CPU with overlap keeps this diagnostic lane bounded.
        command=["srun","--overlap","--exact","--nodes=1","--ntasks=1","--cpus-per-task=1",f"--relative={route['fromNodeRank']}",
                 "python3","-c",probe,address,str(port),transport]
        result=subprocess.run(command,text=True,capture_output=True,check=False)
        if result.returncode == 0: reachable += 1
        else: closed.append(port)
    observations[transport]={"status":"PASS" if not closed else "FAIL","closedPorts":closed,"reachableRoutes":reachable}
try:
    verdict=evaluate_transport_probe(process_map,observations)
    status=0
except TopologyError as exc:
    verdict={"status":"FAIL","reasonCode":str(exc)};status=4
Path(sys.argv[2]).write_text(json.dumps({"processMap":process_map,"observations":observations,"verdict":verdict},indent=2,sort_keys=True)+"\n")
raise SystemExit(status)
PY
