#!/bin/bash
set -Eeuo pipefail
: "${SPEC160_RANK:?}"
: "${SPEC160_PORT:?}"
rank=$SPEC160_RANK

wait_file()
{
  path=$1
  for _ in $(seq 1 1200); do
    test -f "$path" && return 0
    sleep 0.1
  done
  echo "BARRIER_TIMEOUT:$path" >&2
  return 5
}

wait_all()
{
  stem=$1
  wait_file "/shared/${stem}-0"
  wait_file "/shared/${stem}-1"
  wait_file "/shared/${stem}-2"
}

cleanup()
{
  rc=$?
  trap - EXIT INT TERM
  nfdc status report > /scratch/log/nfd-status-exit.txt 2>/dev/null || true
  if test -n "${nfd_pid:-}"; then kill "$nfd_pid" 2>/dev/null || true; fi
  wait "${nfd_pid:-}" 2>/dev/null || true
  exit "$rc"
}
trap cleanup EXIT INT TERM

touch "/shared/node-info-${rank}"
wait_all node-info
ip0=$(</shared/node-0/ipv4.txt)
ip1=$(</shared/node-1/ipv4.txt)
raw_udp_port=$((SPEC160_PORT + 1))
case "$rank" in
  0)
    /source/raw-udp-probe.py listen 0.0.0.0 "$raw_udp_port" \
      rank1-to-rank0 "/shared/raw-udp-listener-0-ready" \
      > /shared/raw-udp-rank0.log 2>&1
    ;;
  1)
    /source/raw-udp-probe.py listen 0.0.0.0 "$raw_udp_port" \
      rank2-to-rank1 "/shared/raw-udp-listener-1-ready" \
      > /shared/raw-udp-rank1-listen.log 2>&1 &
    raw_listener_pid=$!
    sleep 20
    /source/raw-udp-probe.py send "$ip0" "$raw_udp_port" rank1-to-rank0 \
      > /shared/raw-udp-rank1-send.log 2>&1
    wait "$raw_listener_pid"
    ;;
  2)
    sleep 40
    /source/raw-udp-probe.py send "$ip1" "$raw_udp_port" rank2-to-rank1 \
      > /shared/raw-udp-rank2-send.log 2>&1
    ;;
esac
touch "/shared/raw-udp-done-${rank}"
wait_all raw-udp-done

nfd --config /scratch/nfd.conf > /scratch/log/nfd.log 2>&1 &
nfd_pid=$!
for _ in $(seq 1 100); do
  test -S /scratch/run/nfd.sock && nfdc status >/dev/null 2>&1 && break
  kill -0 "$nfd_pid" 2>/dev/null || exit 5
  sleep 0.1
done
test -S /scratch/run/nfd.sock
nfdc status report > /scratch/log/nfd-status-initial.txt
touch "/shared/nfd-ready-${rank}"
sleep 30

add_route()
{
  peer=$1
  transport=$2
  prefix=$3
  uri="${transport}4://${peer}:${SPEC160_PORT}"
  nfdc face create "$uri"
  nfdc route add "$prefix" "$uri" origin 65 cost 0
}
for transport in tcp udp; do
  prefix="/spec160/${transport}"
  case "$rank" in
    0) : ;;
    1) add_route "$ip0" "$transport" "$prefix" ;;
    2) add_route "$ip1" "$transport" "$prefix" ;;
  esac
done
touch "/shared/routes-ready-${rank}"
sleep 60

for transport in tcp udp; do
  name="/spec160/${transport}/${SLURM_JOB_ID}"
  payload="spec160-${transport}-${SLURM_JOB_ID}"
  if test "$rank" -eq 0; then
    /source/ndn-data-probe producer "$name" "$payload" \
      "/shared/${transport}-producer-ready" \
      > "/shared/${transport}-producer.log" 2>&1 &
    producer_pid=$!
    wait "$producer_pid"
    touch "/shared/${transport}-producer-done"
  elif test "$rank" -eq 2; then
    sleep 30
    /source/ndn-data-probe consumer "$name" "$payload" \
      > "/shared/${transport}-consumer.log" 2>&1
    touch "/shared/${transport}-consumer-done"
  fi
  wait_file "/shared/${transport}-producer-done"
  wait_file "/shared/${transport}-consumer-done"
done

nfdc status report > /scratch/log/nfd-status-final.txt
touch "/shared/rank-complete-${rank}"
