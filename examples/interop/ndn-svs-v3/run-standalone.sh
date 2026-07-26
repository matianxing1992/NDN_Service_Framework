#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CPP=${CPP_PEER:-$ROOT/build/svs3-peer}
NDNTS=${NDNTS_PEER:-$ROOT/ndnts/svs3-peer.ts}
NODE=${NODE_BIN:-/home/tianxing/.local/node-v22.23.1/bin/node}
NDN_SVS_SOURCE=${NDN_SVS_SOURCE:-/home/tianxing/NDN/ndn-svs}
PREFIX=${SYNC_PREFIX:-/ndn/ndnsf/svs-v3-interop}
COUNT=${PUBLISH_COUNT:-5}
OUT=${OUTPUT_DIR:-/tmp/ndnsf-svs-v3-interop}
UPLINK=${NDNTS_UPLINK:-unix:///run/nfd/nfd.sock}

test -x "$CPP"
test -f "$NDNTS"
test -x "$NODE"
mkdir -p "$OUT"

started_nfd=0
if ! nfdc status >/dev/null 2>&1; then
  nfd-start >"$OUT/nfd-start.log" 2>&1
  started_nfd=1
  for _ in $(seq 1 40); do
    nfdc status >/dev/null 2>&1 && break
    sleep 0.1
  done
fi

cleanup() {
  jobs -pr | xargs -r kill 2>/dev/null || true
  if ((started_nfd)); then
    nfd-stop >"$OUT/nfd-stop.log" 2>&1 || true
  fi
}
trap cleanup EXIT
nfdc strategy set "$PREFIX" /localhost/nfd/strategy/multicast >/dev/null

run_case() {
  local case_id=$1 version_cpp=$2 version_ts=$3 count_cpp=$4 count_ts=$5 expected_cpp=$6 expected_ts=$7
  local case_prefix="$PREFIX/$case_id" case_dir="$OUT/$case_id"
  mkdir -p "$case_dir"

  LD_LIBRARY_PATH="$NDN_SVS_SOURCE/build${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$CPP" --version "$version_cpp" --sync-prefix "$case_prefix" --node-prefix /cpp \
    --publish-count "$count_cpp" --publish-interval-ms 20 --start-delay-ms 1500 \
    --settle-ms 1200 --events "$case_dir/cpp.jsonl" \
    >"$case_dir/cpp.stdout" 2>"$case_dir/cpp.stderr" &
  local cpp_pid=$!
  NDNTS_UPLINK="$UPLINK" "$NODE" "$NDNTS" --version "$version_ts" \
    --sync-prefix "$case_prefix" --node-prefix /ndnts --publish-count "$count_ts" \
    --publish-interval-ms 20 --start-delay-ms 1500 --settle-ms 1200 \
    --events "$case_dir/ndnts.jsonl" \
    >"$case_dir/ndnts.stdout" 2>"$case_dir/ndnts.stderr" &
  local ts_pid=$!

  wait "$cpp_pid"
  wait "$ts_pid"

  python3 - "$case_dir" "$expected_cpp" "$expected_ts" <<'PY'
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
expected = {"cpp": int(sys.argv[2]), "ndnts": int(sys.argv[3])}
actual = {}
implementations = {}
for peer in ("cpp", "ndnts"):
    events = [json.loads(line) for line in (root / f"{peer}.jsonl").read_text().splitlines()]
    rejects = [event for event in events if event["event"] == "reject"]
    if rejects:
        raise SystemExit(f"{root.name}: {peer} rejects: {rejects}")
    ranges = [(event["low"], event["high"]) for event in events if event["event"] == "update"]
    actual[peer] = sum(high - low + 1 for low, high in ranges)
    implementations[peer] = sorted({event["implementation"] for event in events})
if actual != expected:
    raise SystemExit(f"{root.name}: expected={expected} actual={actual}")
if implementations.get("ndnts") != ["ndnts-typescript"]:
    raise SystemExit(f"{root.name}: TypeScript peer identity missing: {implementations}")
(root / "summary.json").write_text(json.dumps({"case": root.name, "expected": expected,
                                                "actual": actual, "implementations": implementations,
                                                "passed": True}, indent=2) + "\n")
PY
}

run_case cpp-to-ndnts v3 v3 "$COUNT" 0 0 "$COUNT"
run_case ndnts-to-cpp v3 v3 0 "$COUNT" "$COUNT" 0
run_case concurrent v3 v3 "$COUNT" "$COUNT" "$COUNT" "$COUNT"
run_case explicit-v2 v2 v2 "$COUNT" "$COUNT" "$COUNT" "$COUNT"
run_case profile-mismatch v2 v3 "$COUNT" "$COUNT" 0 0

python3 - "$OUT" <<'PY'
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
cases = [json.loads(path.read_text()) for path in sorted(root.glob("*/summary.json"))]
summary = {"passed": len(cases) == 5 and all(case["passed"] for case in cases),
           "cases": cases, "syncAckCount": 0, "ndntsSourceLanguage": "TypeScript"}
(root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary, sort_keys=True))
if not summary["passed"]:
    raise SystemExit(1)
PY
