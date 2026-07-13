#!/bin/sh
set -eu

usage() { echo "usage: $0 --release RELEASE_DIRECTORY --output NEW_DIRECTORY" >&2; exit 2; }
release=
output=
while [ "$#" -gt 0 ]; do
  case "$1" in
    --release) release=$2; shift 2 ;;
    --output) output=$2; shift 2 ;;
    *) usage ;;
  esac
done
if [ -z "$release" ] || [ -z "$output" ]; then usage; fi
[ -d "$release" ] || { echo "ROOTFS_RELEASE_MISSING:$release" >&2; exit 2; }
[ ! -e "$output" ] || { echo "ROOTFS_OUTPUT_EXISTS:$output" >&2; exit 2; }
complete=false
libraries=
cleanup()
{
  [ -z "$libraries" ] || rm -f "$libraries"
  [ "$complete" = true ] || rm -rf "$output"
}
trap cleanup EXIT
trap 'exit 1' INT TERM
if [ ! -f "$release/release.json" ] || [ ! -f "$release/SHA256SUMS" ]; then
  echo "ROOTFS_RELEASE_MANIFEST_MISSING" >&2; exit 4;
fi
(cd "$release" && sha256sum --check SHA256SUMS >/dev/null) || {
  echo "ROOTFS_RELEASE_DIGEST_INVALID" >&2; exit 4;
}

release_id=$(python3 - "$release/release.json" <<'PY'
import json,re,sys
value=json.load(open(sys.argv[1], encoding="utf-8"))
release_id=value.get("releaseId")
if not isinstance(release_id,str) or not re.fullmatch(r"[A-Za-z0-9._-]+", release_id):
    raise SystemExit("ROOTFS_RELEASE_ID_INVALID")
print(release_id)
PY
)
root="$output"
destination="$root/opt/ndnsf-di/releases/$release_id"
mkdir -p "$destination" "$root/manifest"
cp -a "$release/." "$destination/"
ln -s "releases/$release_id" "$root/opt/ndnsf-di/current"

required='App_ServiceController di-native-provider ndn-repo-ng nfd nfdc'
for name in $required; do
  [ -x "$destination/bin/$name" ] || {
    echo "ROOTFS_REQUIRED_BINARY_MISSING:$name" >&2
    rm -rf "$output"
    exit 4
  }
done

libraries=$(mktemp "${TMPDIR:-/tmp}/ndnsf-rootfs-libraries.XXXXXX")
for binary in "$destination"/bin/*; do
  [ -x "$binary" ] || continue
  ldd "$binary" 2>/dev/null | awk '
    /=> \/[^ ]+/ {print $3}
    /^[[:space:]]*\/[^ ]+/ {print $1}
  ' >> "$libraries" || true
done
sort -u "$libraries" | while IFS= read -r library; do
  [ -f "$library" ] || { echo "ROOTFS_LIBRARY_MISSING:$library" >&2; exit 4; }
  cp -L --parents "$library" "$root"
done

if [ -f /etc/ssl/certs/ca-certificates.crt ]; then
  mkdir -p "$root/etc/ssl/certs"
  cp /etc/ssl/certs/ca-certificates.crt "$root/etc/ssl/certs/"
fi
(cd "$root" && find . -type f ! -path './manifest/SHA256SUMS' -print0 \
  | sort -z | xargs -0 sha256sum > manifest/SHA256SUMS)
complete=true
echo "ROOTFS_PREPARE_PASS releaseId=$release_id output=$output"
