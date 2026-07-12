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
if [ -z "$root" ] && command -v systemctl >/dev/null 2>&1; then
  systemctl disable --now ndnsf-di-bench.service ndnsf-di-providers.target \
    ndnsf-di-controller.target ndnsf-di-controller.service >/dev/null 2>&1 || true
fi
rm -f "$root/opt/ndnsf-di/current" "$root/opt/ndnsf-di/previous"
rm -f \
  "$root/etc/systemd/system/ndnsf-di-controller.service" \
  "$root/etc/systemd/system/ndnsf-di-provider@.service" \
  "$root/etc/systemd/system/ndnsf-di-repo@.service" \
  "$root/etc/systemd/system/ndnsf-di-bench.service" \
  "$root/etc/systemd/system/ndnsf-di-controller.target" \
  "$root/etc/systemd/system/ndnsf-di-providers.target" \
  "$root/usr/lib/tmpfiles.d/ndnsf-di.conf" \
  "$root/etc/logrotate.d/ndnsf-di"
if [ "$purge" = true ]; then
  rm -rf "$root/var/lib/ndnsf-di/cache"
fi
if [ -z "$root" ] && command -v systemctl >/dev/null 2>&1; then
  systemctl daemon-reload
fi
echo "uninstalled activation links; authoritative Repo preserved at $root/var/lib/ndnsf-repo"
