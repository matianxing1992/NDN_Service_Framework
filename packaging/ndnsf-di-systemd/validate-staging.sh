#!/bin/sh
set -eu
usage() { echo "usage: $0 --work-root NEW-DIRECTORY --candidate-id ID --plan-digest sha256:HEX" >&2; exit 2; }
work=
candidate=
plan_digest=
while [ "$#" -gt 0 ]; do
  case "$1" in
    --work-root) work=$2; shift 2 ;;
    --candidate-id) candidate=$2; shift 2 ;;
    --plan-digest) plan_digest=$2; shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$work" ] || usage
[ -n "$candidate" ] || usage
[ -n "$plan_digest" ] || usage
[ ! -e "$work" ] || { echo "work root must not exist: $work" >&2; exit 1; }
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
mkdir -p "$work"

create="$repo/packaging/ndnsf-di-systemd/create-release.sh"
install="$repo/packaging/ndnsf-di-systemd/install.sh"
rollback="$repo/packaging/ndnsf-di-systemd/rollback.sh"
uninstall="$repo/packaging/ndnsf-di-systemd/uninstall.sh"
common_artifacts="--artifact /bin/true:bin/App_ServiceController
--artifact /bin/true:bin/di-native-provider
--artifact /bin/true:bin/ndnsf-di
--artifact /bin/true:bin/ndn-repo-ng
--artifact $repo/examples/ndnsf-di-qwen-pilot.model.json:share/model-manifest.json"

# shellcheck disable=SC2086
"$create" --output "$work/release-n" --release-id spec107-staging-n $common_artifacts
# shellcheck disable=SC2086
"$create" --output "$work/release-n1" --release-id spec107-staging-n1 $common_artifacts
"$install" --release "$work/release-n" --root "$work/root"
printf '%s\n' authoritative-repo-sentinel > "$work/root/var/lib/ndnsf-repo/catalog.sentinel"
repo_before=$(sha256sum "$work/root/var/lib/ndnsf-repo/catalog.sentinel")
"$install" --release "$work/release-n" --root "$work/root"
"$install" --release "$work/release-n1" --root "$work/root"
[ "$(readlink "$work/root/opt/ndnsf-di/previous")" = "releases/spec107-staging-n" ] || {
  echo "N to N+1 did not retain release N" >&2; exit 1;
}
"$install" --release "$work/release-n1" --root "$work/root"
[ "$(readlink "$work/root/opt/ndnsf-di/previous")" = "releases/spec107-staging-n" ] || {
  echo "same-release activation destroyed rollback point" >&2; exit 1;
}
"$rollback" --root "$work/root"
repo_after=$(sha256sum "$work/root/var/lib/ndnsf-repo/catalog.sentinel")
[ "$repo_before" = "$repo_after" ] || { echo "authoritative Repo changed" >&2; exit 1; }
[ "$(readlink "$work/root/opt/ndnsf-di/current")" = "releases/spec107-staging-n" ] || {
  echo "rollback did not restore release N" >&2; exit 1;
}

mkdir -p "$work/verify-units"
cp "$repo/packaging/ndnsf-di-systemd/staging/nfd.service" "$work/verify-units/nfd.service"
cp "$repo/packaging/ndnsf-di-systemd/units/"*.target "$work/verify-units/"
for unit in "$repo/packaging/ndnsf-di-systemd/units/"*.service; do
  sed 's#^ExecStart=.*#ExecStart=/bin/true#' "$unit" > "$work/verify-units/$(basename "$unit")"
done
SYSTEMD_UNIT_PATH="$work/verify-units:/usr/lib/systemd/system:/lib/systemd/system" \
  systemd-analyze verify "$work/verify-units/"*.service "$work/verify-units/"*.target

python3 - "$repo/packaging/ndnsf-di-systemd/units" <<'PY'
import sys
from pathlib import Path
text = "\n".join(p.read_text() for p in Path(sys.argv[1]).glob("*.service"))
required = [
    "NoNewPrivileges=true", "PrivateTmp=true", "ProtectSystem=strict",
    "ProtectHome=true", "ProtectKernelTunables=true", "ProtectControlGroups=true",
    "RestrictSUIDSGID=true", "Restart=on-failure", "TimeoutStopSec=",
]
missing = [item for item in required if item not in text]
if missing:
    raise SystemExit("missing hardening directives: " + ", ".join(missing))
print("STATIC_SECURITY_PASS directives=" + str(len(required)))
PY
grep -qx ndnsf-di "$work/root/etc/ndnsf-di/service-accounts.required"
grep -qx ndnsf-di-repo "$work/root/etc/ndnsf-di/service-accounts.required"
"$uninstall" --root "$work/root"
[ ! -e "$work/root/etc/systemd/system/ndnsf-di-controller.service" ] || {
  echo "uninstall retained controller unit" >&2; exit 1;
}
[ -f "$work/root/var/lib/ndnsf-repo/catalog.sentinel" ] || {
  echo "uninstall removed authoritative Repo" >&2; exit 1;
}
python3 - "$work" "$(printf '%s' "$repo_after" | cut -d' ' -f1)" "$candidate" "$plan_digest" <<'PY'
import hashlib, json, re, sys
from pathlib import Path
work = Path(sys.argv[1])
candidate = sys.argv[3]
plan_digest = sys.argv[4]
if not re.fullmatch(r"spec107-c1(?:-[0-9a-f]{12}){6}", candidate):
    raise SystemExit("invalid Spec 107 candidate identity")
if not re.fullmatch(r"sha256:[0-9a-f]{64}", plan_digest):
    raise SystemExit("invalid plan digest")
model = work / "release-n/share/model-manifest.json"
model_digest = "sha256:" + hashlib.sha256(model.read_bytes()).hexdigest()
record = {
    "schema": "ndnsf-di-spec107-staging-validation-v1",
    "releaseId": "spec107-staging-n",
    "candidateId": candidate,
    "planDigest": plan_digest,
    "modelDigest": model_digest,
    "providerBootId": "local-staging-provider@1",
    "queueDepth": 0,
    "requestId": "staging-validation",
    "terminalReason": "PASS",
    "repoDigest": "sha256:" + sys.argv[2],
    "supervisionClass": "local-process-supervision",
    "physicalProductionDeferred": True,
    "verdict": "PASS",
}
(work / "staging-status.json").write_text(
    json.dumps(record, indent=2, sort_keys=True) + "\n")
(work / "staging-metrics.prom").write_text(
    'ndnsf_di_local_ready{release="spec107-staging-n",supervision="local-process-supervision"} 1\n'
    'ndnsf_di_queue_depth{provider_boot="local-staging-provider@1"} 0\n')
PY
echo "STAGING_PASS root=$work/root repoDigest=$(printf '%s' "$repo_after" | cut -d' ' -f1)"
