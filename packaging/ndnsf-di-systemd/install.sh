#!/bin/sh
set -eu

usage() { echo "usage: $0 --release DIR [--root DIR] [--activate-only]" >&2; exit 2; }
release=
root=
activate_only=false
while [ "$#" -gt 0 ]; do
  case "$1" in
    --release) release=$2; shift 2 ;;
    --root) root=$2; shift 2 ;;
    --activate-only) activate_only=true; shift ;;
    *) usage ;;
  esac
done
[ -n "$release" ] || usage
release=$(CDPATH= cd -- "$release" && pwd)
[ -f "$release/release-manifest.json" ] && [ -f "$release/SHA256SUMS" ] || {
  echo "release manifest or digest file missing" >&2; exit 1;
}
(cd "$release" && sha256sum -c SHA256SUMS)
release_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["releaseId"])' "$release/release-manifest.json")
case "$release_id" in *[!A-Za-z0-9._-]*|'') echo "unsafe release id" >&2; exit 1 ;; esac

prefix="$root/opt/ndnsf-di"
destination="$prefix/releases/$release_id"
mkdir -p "$prefix/releases" "$root/etc/ndnsf-di" "$root/var/lib/ndnsf-di/cache" "$root/var/log/ndnsf-di"
# Authoritative Repo state is deliberately outside the release tree and is never copied or removed.
mkdir -p "$root/var/lib/ndnsf-repo"
if [ "$activate_only" = false ]; then
  if [ -e "$destination" ]; then
    cmp "$release/release-manifest.json" "$destination/release-manifest.json" >/dev/null || {
      echo "release id exists with a different manifest" >&2; exit 1;
    }
  else
    cp -a "$release" "$destination"
  fi
fi
[ -d "$destination" ] || { echo "installed release missing: $destination" >&2; exit 1; }
if [ -L "$prefix/current" ]; then
  old=$(readlink "$prefix/current")
  ln -sfn "$old" "$prefix/previous"
fi
ln -sfn "releases/$release_id" "$prefix/current"
echo "activated $release_id; authoritative Repo preserved at $root/var/lib/ndnsf-repo"
