#!/bin/bash
set -Eeuo pipefail
action=$1
port=$2
shift 2
host_scratch="/tmp/${USER}/spec160-probe-${SLURM_JOB_ID}"

run_in_container()
{
  apptainer exec --nv --cleanenv --containall \
    --home "$host_scratch/home:/home/${USER}" \
    --bind "$host_scratch:/scratch:rw" \
    --bind "$SPEC160_SHARED/source:/source:ro" \
    --env "NDN_CLIENT_TRANSPORT=unix:///scratch/run/nfd.sock,HOME=/home/${USER}" \
    "$SPEC160_SIF" "$@"
}

case "$action" in
  route)
    peer=$1
    transport=$2
    prefix=$3
    uri="${transport}4://${peer}:${port}"
    run_in_container nfdc face create "$uri"
    run_in_container nfdc route add "$prefix" "$uri" origin 65 cost 0
    ;;
  producer)
    name=$1
    payload=$2
    run_in_container /source/ndn-data-probe producer "$name" "$payload"
    ;;
  consumer)
    name=$1
    payload=$2
    run_in_container /source/ndn-data-probe consumer "$name" "$payload"
    ;;
  status)
    run_in_container nfdc status report
    ;;
  *)
    echo "unknown action: $action" >&2
    exit 2
    ;;
esac
