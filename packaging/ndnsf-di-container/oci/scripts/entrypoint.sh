#!/bin/sh
set -eu

release_root=${NDNSF_RELEASE_ROOT:-/opt/ndnsf-di/current}

resolve_binary()
{
  name=$1
  if [ -x "$release_root/bin/$name" ]; then
    printf '%s\n' "$release_root/bin/$name"
    return 0
  fi
  command -v "$name" 2>/dev/null || {
    echo "CONTAINER_BINARY_NOT_FOUND:$name" >&2
    return 127
  }
}

prepare_identity()
{
  source=${NDNSF_IDENTITY_SOURCE:-}
  target=${HOME:-/run/ndnsf-keychain}
  [ -n "$source" ] || return 0
  [ -d "$source" ] || { echo "CONTAINER_IDENTITY_SOURCE_MISSING:$source" >&2; return 4; }
  mkdir -p "$target"
  if [ -z "$(find "$target" -mindepth 1 -maxdepth 1 -print -quit)" ]; then
    cp -a "$source/." "$target/"
  fi
  chmod 0700 "$target"
}

role=${1:-}
[ -n "$role" ] || { echo "usage: entrypoint ROLE [ARGS...]" >&2; exit 2; }
shift

case "$role" in
  nfd)
    binary=$(resolve_binary nfd)
    exec "$binary" --config "${NDNSF_NFD_CONFIG:-/etc/ndn/nfd.conf}" "$@"
    ;;
  controller)
    prepare_identity
    binary=$(resolve_binary App_ServiceController)
    exec "$binary" "$@"
    ;;
  provider)
    prepare_identity
    binary=$(resolve_binary di-native-provider)
    exec "$binary" "$@"
    ;;
  repo)
    prepare_identity
    binary=$(resolve_binary ndn-repo-ng)
    exec "$binary" "$@"
    ;;
  cli)
    prepare_identity
    binary=$(resolve_binary ndnsf-di)
    exec "$binary" "$@"
    ;;
  exec)
    [ "$#" -gt 0 ] || { echo "entrypoint exec requires a command" >&2; exit 2; }
    exec "$@"
    ;;
  *)
    echo "CONTAINER_ROLE_INVALID:$role" >&2
    exit 2
    ;;
esac
