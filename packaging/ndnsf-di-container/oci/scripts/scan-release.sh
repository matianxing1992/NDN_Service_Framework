#!/bin/sh
set -eu

usage() { echo "usage: $0 --path DIRECTORY" >&2; exit 2; }
root=
while [ "$#" -gt 0 ]; do
  case "$1" in
    --path) root=$2; shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$root" ] || usage
[ -d "$root" ] || { echo "SECRET_SCAN_PATH_INVALID:$root" >&2; exit 2; }

name_findings=$(find "$root" -type f \( \
  -name '.env' -o -name '.env.*' -o -name '*.key' -o -name '*.pem' \
  -o -name '*.p12' -o -name '*.pfx' -o -name '*.sif' \) -print)
private_marker='-----BEGIN .*PRIVATE'' KEY-----'
cloud_marker='AWS_SECRET_ACCESS_''KEY[[:space:]]*='
password_marker='password[[:space:]]*[=:][[:space:]]*[^[:space:]]'
content_findings=$(find "$root" -type f -size -8M \
  -exec grep -IEl "$private_marker|$cloud_marker|$password_marker" -- {} + 2>/dev/null || true)

if [ -n "$name_findings$content_findings" ]; then
  echo "SECRET_SCAN_FAIL" >&2
  [ -z "$name_findings" ] || printf '%s\n' "$name_findings" >&2
  [ -z "$content_findings" ] || printf '%s\n' "$content_findings" >&2
  exit 4
fi
count=$(find "$root" -type f | wc -l)
echo "SECRET_SCAN_PASS files=$count"
