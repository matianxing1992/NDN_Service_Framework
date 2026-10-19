#!/usr/bin/env bash
set -euo pipefail

binary=/usr/local/bin/uav-video-pipeline-probe
if [[ ! -x "$binary" ]]; then
  echo 'Install the Waf uav-video-pipeline-probe target before running this probe.' >&2
  exit 1
fi
exec "$binary" "$@"
