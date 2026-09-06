#!/usr/bin/env bash
# Spec179 T011d MiniNDN campaign driver.
# Runs every launcher scenario as a privileged, isolated MiniNDN run and
# writes per-scenario result.json + logs under ROOT/results/spec179-minindn/<s>.
# Usage: scripts/spec179_minindn_campaign.sh [scenario ...]
#   (no args: all SCENARIOS in fixed order; "--dry" lists commands only)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2
OUT="${NDNSF_CAMPAIGN_OUTPUT:-${ROOT}/results/spec179-minindn-$(date +%Y%m%d-%H%M%S)}"
LAUNCHER="tests/minindn/run_request_scoped_confidentiality.py"
BUILD_DIR="${NDNSF_BUILD_DIR:-build-clang-spec179-rv32}"
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
  grant-after-permission-exhaustion
  provider-grant-only-advance
  provider-grant-after-permission-exhaustion
  revocation-rotation-failure-retry
)
if [[ "${1:-}" == "--dry" ]]; then
  for s in "${ALL[@]}"; do
    echo "sudo -n python3 $LAUNCHER --execute --scenario $s --output $OUT/$s --build-dir $BUILD_DIR"
  done
  exit 0
fi
if (( $# > 0 )); then
  SCENS=("$@")
else
  SCENS=("${ALL[@]}")
fi
for s in "${SCENS[@]}"; do
  if [[ ! " ${ALL[*]} " == *" $s "* ]]; then
    echo "unknown scenario: $s" >&2
    exit 2
  fi
  if [[ -e "$OUT/$s" ]]; then
    echo "refusing to overwrite existing scenario evidence: $OUT/$s" >&2
    exit 2
  fi
done
mkdir -p "$OUT"
: > "$OUT/campaign-summary.tsv"
printf 'scenario\tgatePassed\tstatus\trevocationApplied\tgrantOnlyGateOk\tnetworkEvidence\texecutionCount\trunDurationSec\texit\n' \
  >> "$OUT/campaign-summary.tsv"
campaign_rc=0
for s in "${SCENS[@]}"; do
  dir="$OUT/$s"
  mkdir -p "$dir"
  started=$(date +%s)
  sudo -n python3 "$LAUNCHER" --execute --scenario "$s" \
    --output "$dir" --build-dir "$BUILD_DIR" > "$dir/run.stdout.json" 2>&1
  rc=$?
  if (( rc != 0 )); then campaign_rc=1; fi
  ended=$(date +%s)
  # Compact per-scenario gate summary appended as TSV for the evidence pass.
  python3 - "$dir/result.json" "$rc" "$((ended - started))" "$OUT/campaign-summary.tsv" <<'PYEOF'
import json,sys
path,rc,dur,tsv=sys.argv[1],int(sys.argv[2]),int(sys.argv[3]),sys.argv[4]
r={}
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
sys.exit(0 if rc == 0 and r.get("gatePassed") is True else 1)
PYEOF
  if (( $? != 0 )); then campaign_rc=1; fi
done
sudo -n chown -R "$(id -u):$(id -g)" "$OUT" 2>/dev/null || true
echo "campaign done -> $OUT/campaign-summary.tsv"
exit "$campaign_rc"
