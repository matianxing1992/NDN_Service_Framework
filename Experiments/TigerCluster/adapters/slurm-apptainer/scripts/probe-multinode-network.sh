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

PYTHONPATH="$lib" python3 - "$process_map" "$output" <<'PY'
import json,os,subprocess,sys
from pathlib import Path
from allocation_topology import TopologyError,evaluate_transport_probe,load_process_map

process_map=load_process_map(sys.argv[1])
addresses=[node["address"] for node in process_map["nodes"]]
observations={"allocationAddresses":addresses}
for transport in ("tcp","udp"):
    closed=[];reachable=0
    for route in process_map["routes"]:
        if route["transport"] != process_map["selectedTransport"]:
            continue
        command=["srun","--exclusive","--nodes=1","--ntasks=1",f"--relative={route['fromNodeRank']}",
                 "nc","-z","-w","2"]
        if transport == "udp": command.append("-u")
        command.extend([route["remoteAddress"],str(route["port"])])
        result=subprocess.run(command,text=True,capture_output=True,check=False)
        if result.returncode == 0: reachable += 1
        else: closed.append(route["port"])
    observations[transport]={"status":"PASS" if not closed else "FAIL","closedPorts":closed,"reachableRoutes":reachable}
try:
    verdict=evaluate_transport_probe(process_map,observations)
    status=0
except TopologyError as exc:
    verdict={"status":"FAIL","reasonCode":str(exc)};status=4
Path(sys.argv[2]).write_text(json.dumps({"processMap":process_map,"observations":observations,"verdict":verdict},indent=2,sort_keys=True)+"\n")
raise SystemExit(status)
PY
