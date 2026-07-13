#!/bin/sh
set -eu

mode=${1:-application}
socket_path=${NDNSF_NFD_SOCKET:-/run/nfd/nfd.sock}

case "$mode" in
  nfd)
    [ -S "$socket_path" ] || { echo "NFD_SOCKET_NOT_READY:$socket_path" >&2; exit 1; }
    export NDN_CLIENT_TRANSPORT=${NDN_CLIENT_TRANSPORT:-unix://$socket_path}
    nfdc status >/dev/null
    ;;
  application)
    [ -S "$socket_path" ] || { echo "NFD_SOCKET_NOT_READY:$socket_path" >&2; exit 1; }
    kill -0 1
    ;;
  *)
    echo "HEALTHCHECK_MODE_INVALID:$mode" >&2
    exit 2
    ;;
esac
