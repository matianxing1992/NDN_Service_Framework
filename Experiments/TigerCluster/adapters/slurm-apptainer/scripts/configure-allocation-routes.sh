#!/bin/bash
set -euo pipefail

usage() { echo "usage: $0 --process-map FILE --evidence DIR" >&2; exit 2; }
process_map= evidence=
while (($#)); do
  case "$1" in
    --process-map) process_map=$2; shift 2 ;;
    --evidence) evidence=$2; shift 2 ;;
    *) usage ;;
  esac
done
[[ -n $process_map && -f $process_map && -n $evidence ]] || usage
[[ -n ${SLURM_JOB_ID:-} || ${NDNSF_SPEC110_TEST_MODE:-0} == 1 ]] || {
  echo SPEC110_ROUTE_CONFIG_REQUIRES_ALLOCATION >&2; exit 3;
}
container_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
lib="$container_root/lib"; mkdir -p "$evidence"
routes="$evidence/routes.tsv"
PYTHONPATH="$lib" python3 - "$process_map" >"$routes" <<'PY'
import sys
from allocation_topology import load_process_map
value=load_process_map(sys.argv[1]);nodes=value['nodes']
for route in value['routes']:
 node=nodes[route['fromNodeRank']]
 print(route['fromNodeRank'],node['nfdSocket'],route['prefix'],route['transport'],route['remoteAddress'],route['port'],sep='\t')
PY

: >"$evidence/route-commands.log"
while IFS=$'\t' read -r from_rank socket prefix transport address port; do
  [[ -n $from_rank ]] || continue
  uri="${transport}4://${address}:${port}"
  command=(srun --exclusive --nodes=1 --ntasks=1 "--relative=$from_rank" env "NDN_CLIENT_TRANSPORT=unix://$socket" nfdc)
  printf '%q ' "${command[@]}" face create remote "$uri" persistency permanent >>"$evidence/route-commands.log"; printf '\n' >>"$evidence/route-commands.log"
  "${command[@]}" face create remote "$uri" persistency permanent >>"$evidence/nfdc-route.log" 2>&1
  "${command[@]}" route add prefix "$prefix" nexthop "$uri" cost 10 >>"$evidence/nfdc-route.log" 2>&1
  "${command[@]}" face list remote "$uri" >>"$evidence/face-state.txt" 2>&1
  "${command[@]}" route list prefix "$prefix" >>"$evidence/route-state.txt" 2>&1
done <"$routes"
printf 'ROUTE_CONFIGURATION_PASS\n' >"$evidence/route-verdict.txt"
