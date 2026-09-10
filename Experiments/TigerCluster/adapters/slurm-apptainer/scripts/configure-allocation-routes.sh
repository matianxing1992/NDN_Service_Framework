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

# Route commands also use ``--relative=<fromNodeRank>``.  Keep this direct
# entry point subject to the same scheduler-order binding as the supervisor;
# otherwise a standalone route retry could silently configure the wrong NFD on
# a real multi-node allocation even though the supervisor had validated a
# different order.
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
  # Route configuration runs while each node's NFD is already alive.  An
  # exclusive step would reserve the entire node and wait behind that NFD;
  # use one exact CPU with overlap so this short control step can coexist.
  command=(srun --overlap --exact --nodes=1 --ntasks=1 --cpus-per-task=1 "--relative=$from_rank" env "NDN_CLIENT_TRANSPORT=unix://$socket" nfdc)
  printf '%q ' "${command[@]}" face create remote "$uri" persistency permanent >>"$evidence/route-commands.log"; printf '\n' >>"$evidence/route-commands.log"
  "${command[@]}" face create remote "$uri" persistency permanent >>"$evidence/nfdc-route.log" 2>&1
  "${command[@]}" route add prefix "$prefix" nexthop "$uri" cost 10 >>"$evidence/nfdc-route.log" 2>&1
  "${command[@]}" face list remote "$uri" >>"$evidence/face-state.txt" 2>&1
  "${command[@]}" route list prefix "$prefix" >>"$evidence/route-state.txt" 2>&1
done <"$routes"
printf 'ROUTE_CONFIGURATION_PASS\n' >"$evidence/route-verdict.txt"
