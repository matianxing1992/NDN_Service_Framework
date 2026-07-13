#!/bin/sh
set -eu

repo=$(cd -P -- "$(dirname -- "$0")/../.." && pwd)
mode=${1:-offline}

case "$mode" in
  offline)
    exec python3 -m unittest discover -v -s "$repo/tests/container" -p 'test_*.py'
    ;;
  live)
    [ "${NDNSF_CONTAINER_LIVE:-0}" = 1 ] || {
      echo "live tests require NDNSF_CONTAINER_LIVE=1" >&2
      exit 2
    }
    echo "no live tests are implemented in Spec 108 Phase 1-2" >&2
    exit 2
    ;;
  *)
    echo "usage: $0 [offline|live]" >&2
    exit 2
    ;;
esac
