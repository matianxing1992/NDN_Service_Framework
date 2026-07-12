#!/bin/sh
set -eu
usage() { echo "usage: $0 [--root DIR] [--purge-disposable-cache]" >&2; exit 2; }
root=
purge=false
while [ "$#" -gt 0 ]; do
  case "$1" in
    --root) root=$2; shift 2 ;;
    --purge-disposable-cache) purge=true; shift ;;
    *) usage ;;
  esac
done
rm -f "$root/opt/ndnsf-di/current" "$root/opt/ndnsf-di/previous"
if [ "$purge" = true ]; then
  rm -rf "$root/var/lib/ndnsf-di/cache"
fi
echo "uninstalled activation links; authoritative Repo preserved at $root/var/lib/ndnsf-repo"
