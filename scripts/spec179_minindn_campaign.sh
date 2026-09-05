#!/usr/bin/env bash
# Spec179 T011d MiniNDN campaign driver.
# Runs every launcher scenario as a privileged, isolated MiniNDN run and
# writes per-scenario result.json + logs under ROOT/results/spec179-minindn/<s>.
# Usage: scripts/spec179_minindn_campaign.sh [scenario ...]
#   (no args: all SCENARIOS in fixed order; "--dry" lists commands only)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2
OUT="${ROOT}/results/spec179-minindn"
LAUNCHER="tests/minindn/run_request_scoped_confidentiality.py"
BUILD_DIR="build-clang-spec179-nac3"
ALL=(
  user-identity-revocation
  provider-identity-revocation
  service-scoped-revocation-with-unaffected-control
  inflight-revocation
  offline-rejoin-epoch-skip
  controller-cache-provider-status-retrieval
  controller-unavailable-expiry
  large-response-invalidation
  targeted-refill-invalidation
  stream-invalidation
  hintless-scheduled-refresh
  controller-restart
  selection-response-tamper-and-replay
  grant-only-advance
)
if [[ "${1:-}" == "--dry" ]]; then
  for s in "${ALL[@]}"; do
    echo "sudo -n python3 $LAUNCHER --execute --scenario $s --output $OUT/$s --build-dir $BUILD_DIR"
  done
  exit 0
fi
SCENS=("${@:-${ALL[@]}}")
mkdir -p "$OUT"
: > "$OUT/campaign-summary.tsv"
printf 'scenario\tgatePassed\tstatus\trevocationApplied\tgrantOnlyGateOk\tnetworkEvidence\texecutionCount\trunDurationSec\texit\n' \
  >> "$OUT/campaign-summary.tsv"
for s in "${SCENS[@]}"; do
  dir="$OUT/$s"
  rm -rf "$dir"           # keep each scenario dir a single-run evidence unit
  mkdir -p "$dir"
  started=$(date +%s)
  sudo -n python3 "$LAUNCHER" --execute --scenario "$s" \
    --output "$dir" --build-dir "$BUILD_DIR" > "$dir/run.stdout.json" 2>&1
  rc=$?
  ended=$(date +%s)
  # Compact per-scenario gate summary appended as TSV for the evidence pass.
  python3 - "$dir/result.json" "$rc" "$((ended - started))" "$OUT/campaign-summary.tsv" <<'PYEOF'
import json,sys
path,rc,dur,tsv=sys.argv[1],int(sys.argv[2]),int(sys.argv[3]),sys.argv[4]
try:
    r=json.load(open(path))
    line="%(s)s\t%(g)s\t%(st)s\t%(ra)s\t%(gog)s\t%(ne)s\t%(ec)s\t%(du)s\t%(rc)s" % {
      "s":r.get("scenario","?"),"g":r.get("gatePassed"),"st":r.get("status"),
      "ra":r.get("revocationApplied"),"gog":r.get("grantOnlyGateOk"),
      "ne":r.get("networkEvidence"),"ec":r.get("executionCount"),
      "du":dur,"rc":rc}
except Exception as e:
    line="scenario-parse-error\t%s\t%s\t%s" % (e, rc, dur)
with open(tsv,"a") as f:
    f.write(line+"\n")
print(line)
PYEOF
done
sudo -n chown -R "$(id -u):$(id -g)" "$OUT" 2>/dev/null || true
echo "campaign done -> $OUT/campaign-summary.tsv"
