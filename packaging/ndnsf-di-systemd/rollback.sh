#!/bin/sh
set -eu
usage() { echo "usage: $0 [--root DIR]" >&2; exit 2; }
root=
while [ "$#" -gt 0 ]; do
  case "$1" in --root) root=$2; shift 2 ;; *) usage ;; esac
done
prefix="$root/opt/ndnsf-di"
[ -L "$prefix/current" ] && [ -L "$prefix/previous" ] || {
  echo "current/previous release symlinks are required" >&2; exit 1;
}
current=$(readlink "$prefix/current")
previous=$(readlink "$prefix/previous")
[ -d "$prefix/$previous" ] || { echo "previous release is missing" >&2; exit 1; }
ln -sfn "$previous" "$prefix/current"
ln -sfn "$current" "$prefix/previous"
echo "rolled back to $previous; authoritative Repo and catalogs were not modified"
