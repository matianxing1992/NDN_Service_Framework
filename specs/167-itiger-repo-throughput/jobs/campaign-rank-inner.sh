#!/bin/bash
set -Eeuo pipefail
: "${SPEC167_RANK:?}" "${SPEC167_PORT:?}" "${SPEC167_RUN_NAME:?}"
: "${SPEC167_SUBJECT:?}" "${SPEC167_PAYLOAD_SIZE:?}" "${SPEC167_TCP_PORT:?}"
: "${SPEC167_PEER_HOST:?}" "${SPEC167_DURATION_SECONDS:?}"

rank=$SPEC167_RANK
peer_host=$SPEC167_PEER_HOST
shared_out="/shared/runs/${SPEC167_RUN_NAME}"
local_root="/scratch/coord"
runner=/source/Experiments/NDNSF_DistributedRepo_Artifact_Itiger.py
pids=()
cleanup()
{
  rc=$?
  trap - EXIT INT TERM
  for pid in "${pids[@]:-}"; do kill "$pid" 2>/dev/null || true; done
  nfdc status report > /scratch/log/nfd-status-exit.txt 2>/dev/null || true
  if test -n "${nfd_pid:-}"; then kill "$nfd_pid" 2>/dev/null || true; fi
  wait "${nfd_pid:-}" 2>/dev/null || true
  exit "$rc"
}
trap cleanup EXIT INT TERM

wait_file()
{
  local path=$1 attempts="${2:-9000}"
  for _ in $(seq 1 "$attempts"); do
    test -f "$path" && return 0
    sleep 0.1
  done
  echo "LOCAL_BARRIER_TIMEOUT:$path" >&2
  return 5
}
wait_file_or_pid()
{
  local path=$1 pid=$2 attempts="${3:-9000}"
  for _ in $(seq 1 "$attempts"); do
    test -f "$path" && return 0
    if ! kill -0 "$pid" 2>/dev/null; then wait "$pid"; return $?; fi
    sleep 0.1
  done
  return 5
}
run_nfdc() { timeout 30s nfdc "$@"; }
create_peer_face()
{
  local uri=$1
  : > /scratch/log/face-create.txt
  for attempt in $(seq 1 300); do
    if timeout 2s nfdc face create "$uri" >> /scratch/log/face-create.txt 2>&1; then
      return 0
    fi
    printf 'face-create-retry attempt=%s uri=%s\n' "$attempt" "$uri" \
      >> /scratch/log/face-create.txt
    sleep 0.1
  done
  echo "PEER_FACE_CREATE_TIMEOUT:$uri" >&2
  return 5
}
receive_files()
{
  local coord=$1 port=$2
  /opt/venv/bin/python "$runner" --role control-receive \
    --coord-dir "$coord" --host 0.0.0.0 --port "$port" \
    --timeout-seconds 900
}
send_files()
{
  local coord=$1 port=$2 names=$3
  /opt/venv/bin/python "$runner" --role control-send \
    --coord-dir "$coord" --host "$peer_host" --port "$port" \
    --file-names "$names" --timeout-seconds 900
}

mkdir -p "$local_root" /scratch/data "$shared_out"
export NDN_CLIENT_TRANSPORT=unix:///scratch/run/nfd.sock
export LD_LIBRARY_PATH="/opt/ndn-base/lib:/opt/ndnsf-app/lib:${LD_LIBRARY_PATH:-}"
export PYTHONPATH="/source/Experiments:/opt/ndnsf-app/python"
export PYTHONUNBUFFERED=1
sleep "${SPEC167_STARTUP_DELAY_SECONDS:-0}"
nfd --config /scratch/nfd.conf > /scratch/log/nfd.log 2>&1 &
nfd_pid=$!
for _ in $(seq 1 300); do
  test -S /scratch/run/nfd.sock && run_nfdc status >/dev/null 2>&1 && break
  kill -0 "$nfd_pid" 2>/dev/null || exit 5
  sleep 0.1
done
uri="tcp4://${peer_host}:${SPEC167_PORT}"
create_peer_face "$uri"
if test "$rank" -eq 0; then
  run_nfdc route add /spec167/cold "$uri" origin 65 cost 0
else
  run_nfdc route add /spec167/source "$uri" origin 65 cost 0
fi
sleep 1

if test "$SPEC167_SUBJECT" = physical-network; then
  tcp_coord="$local_root/tcp"
  mkdir -p "$tcp_coord"
  if test "$rank" -eq 1; then
    /opt/venv/bin/python "$runner" --role tcp-server --coord-dir "$tcp_coord" \
      --host 0.0.0.0 --port "$SPEC167_TCP_PORT" --timeout-seconds 180
    send_files "$tcp_coord" $((SPEC167_TCP_PORT+1)) tcp-result.json
  else
    /opt/venv/bin/python "$runner" --role tcp-client --coord-dir "$tcp_coord" \
      --host "$peer_host" --port "$SPEC167_TCP_PORT" \
      --duration-seconds "$SPEC167_DURATION_SECONDS" --timeout-seconds 180
    receive_files "$tcp_coord" $((SPEC167_TCP_PORT+1))
    cp "$tcp_coord/tcp-result.json" "$shared_out/tcp-result.json"
    touch "$shared_out/rank-complete-0" "$shared_out/rank-complete-1"
  fi
  exit 0
fi

base_coord="$local_root/base"
base_data="/scratch/data/base"
completion_port=$((SPEC167_TCP_PORT+16))
if test "$rank" -eq 0; then
  /opt/venv/bin/python "$runner" --role prepare --coord-dir "$base_coord" \
    --data-dir "$base_data" --payload-size "$SPEC167_PAYLOAD_SIZE" \
    > /scratch/log/prepare.txt
fi

cumulative_ms=0
iteration=0
while :; do
  coord="$local_root/iterations/$iteration"
  data="/scratch/data/iterations/$iteration"
  mkdir -p "$coord"
  object_prefix="/spec167/source/${SPEC167_RUN_NAME}/${iteration}"
  cold_prefix="/spec167/cold/${SPEC167_RUN_NAME}/${iteration}"
  metadata_port=$((SPEC167_TCP_PORT+10))
  store_port=$((SPEC167_TCP_PORT+11))
  ready_port=$((SPEC167_TCP_PORT+12))
  cold_port=$((SPEC167_TCP_PORT+13))
  result_port=$((SPEC167_TCP_PORT+14))
  decision_port=$((SPEC167_TCP_PORT+15))

  if test "$rank" -eq 0; then
    cp "$base_coord"/raw-fixture.json "$base_coord"/manifest.json \
       "$base_coord"/manifest.signature "$base_coord"/manifest-public.der \
       "$base_coord"/manifest-public.pem "$coord/"
    /opt/venv/bin/python "$runner" --role producer --coord-dir "$coord" \
      --data-dir "$base_data" --subject "$SPEC167_SUBJECT" \
      --object-prefix "$object_prefix" --timeout-seconds 900 \
      > "/scratch/log/producer-${iteration}.txt" 2>&1 &
    producer_pid=$!; pids+=("$producer_pid")
    wait_file_or_pid "$coord/benchmark-producer.ready" "$producer_pid"
    send_files "$coord" "$metadata_port" \
      raw-fixture.json,manifest.json,manifest.signature,manifest-public.der,manifest-public.pem,benchmark-producer-info.json,benchmark-producer.ready
    receive_files "$coord" "$store_port"
    receive_files "$coord" "$ready_port"
    /opt/venv/bin/python "$runner" --role cold-consumer \
      --coord-dir "$coord" --data-dir "$data" --result-name result \
      --timeout-seconds 900 > "/scratch/log/cold-${iteration}.txt" 2>&1
    send_files "$coord" "$cold_port" result.cold.json
    if test "$SPEC167_SUBJECT" = digest-only -o "$SPEC167_SUBJECT" = signed-manifest; then
      receive_files "$coord" "$result_port"
    else
      receive_files "$coord" "$result_port"
    fi
    touch "$coord/stop"
    wait "$producer_pid"
    cumulative_ms=$(/opt/venv/bin/python - "$cumulative_ms" "$coord/result.json" <<'PY'
import json, sys
print(float(sys.argv[1]) + float(json.load(open(sys.argv[2]))["elapsedMs"]))
PY
)
    /opt/venv/bin/python - "$coord/decision.json" "$cumulative_ms" "$SPEC167_DURATION_SECONDS" <<'PY'
import json, sys
path, elapsed, duration = sys.argv[1:]
with open(path, "w", encoding="utf-8") as output:
    json.dump({"continue": float(elapsed) < float(duration) * 1000.0}, output)
PY
    send_files "$coord" "$decision_port" decision.json
  else
    receive_files "$coord" "$metadata_port"
    /opt/venv/bin/python "$runner" --role consumer --coord-dir "$coord" \
      --data-dir "$data" --subject "$SPEC167_SUBJECT" \
      --object-prefix "$object_prefix" --cold-prefix "$cold_prefix" \
      --result-name result --timeout-seconds 900 \
      > "/scratch/log/consumer-${iteration}.txt" 2>&1 &
    consumer_pid=$!; pids+=("$consumer_pid")
    wait_file_or_pid "$coord/result.store-ready.json" "$consumer_pid"
    touch "$coord/result.serve-cold"
    send_files "$coord" "$store_port" result.store-ready.json
    wait_file_or_pid "$coord/result.producer-ready.json" "$consumer_pid"
    send_files "$coord" "$ready_port" result.producer-ready.json
    receive_files "$coord" "$cold_port"
    wait "$consumer_pid"
    if test "$SPEC167_SUBJECT" = digest-only -o "$SPEC167_SUBJECT" = signed-manifest; then
      digest=$(/opt/venv/bin/python - "$coord/raw-fixture.json" <<'PY'
import json, sys
print(json.load(open(sys.argv[1]))["contentDigest"])
PY
)
      /opt/venv/bin/python "$runner" --role warm-reuse --coord-dir "$coord" \
        --data-dir "$data" --result-name result --expected-digest "$digest"
      send_files "$coord" "$result_port" result.json,result.warm.json
    else
      send_files "$coord" "$result_port" result.json
    fi
    receive_files "$coord" "$decision_port"
  fi

  continue_run=$(/opt/venv/bin/python - "$coord/decision.json" <<'PY'
import json, sys
print("yes" if json.load(open(sys.argv[1]))["continue"] else "no")
PY
)
  iteration=$((iteration+1))
  test "$continue_run" = yes || break
done

if test "$rank" -eq 0; then
  final="$local_root/final"
  mkdir -p "$final"
  /opt/venv/bin/python - "$local_root/iterations" "$final" "$SPEC167_SUBJECT" <<'PY'
import json, pathlib, sys
root, final, subject = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3]
rows=[]; cold=[]; warm=[]
for path in sorted(root.iterdir(), key=lambda p: int(p.name)):
    rows.append(json.loads((path / "result.json").read_text()))
    cold.append(json.loads((path / "result.cold.json").read_text()))
    warm_path=path / "result.warm.json"
    if warm_path.is_file(): warm.append(json.loads(warm_path.read_text()))
def aggregate(items):
    elapsed=sum(float(x.get("elapsedMs",0)) for x in items)
    logical=sum(int(x.get("logicalBytes",0)) for x in items)
    return {"status":"SUCCESS" if items and all(x.get("status")=="SUCCESS" for x in items) else "FAIL",
            "subject":subject,"subtransfers":len(items),"elapsedMs":elapsed,
            "logicalBytes":logical,"logicalGoodputMbps":logical*8/elapsed/1000 if elapsed else 0.0,
            "destination":"/scratch/data/iterations/*/cold-destinations/result.bin",
            "timeoutCount":sum(int(x.get("timeoutCount",0)) for x in items),
            "retransmissionCount":sum(int(x.get("retransmissionCount",0)) for x in items)}
(final / "result.json").write_text(json.dumps(aggregate(rows),sort_keys=True)+"\n")
(final / "result.cold.json").write_text(json.dumps(aggregate(cold),sort_keys=True)+"\n")
if warm:
    value={"status":"SUCCESS" if all(x.get("status")=="SUCCESS" for x in warm) else "FAIL",
           "subtransfers":len(warm),"duplicatePayloadBytesWritten":sum(int(x.get("duplicatePayloadBytesWritten",0)) for x in warm)}
    (final / "result.warm.json").write_text(json.dumps(value,sort_keys=True)+"\n")
PY
  receive_files "$final" "$completion_port"
  cp "$final"/result*.json "$shared_out/"
  touch "$shared_out/rank-complete-0" "$shared_out/rank-complete-1"
else
  final="$local_root/final"
  mkdir -p "$final"
  printf '{"rank":1,"status":"SUCCESS"}\n' > "$final/rank-1-complete.json"
  send_files "$final" "$completion_port" rank-1-complete.json
fi
