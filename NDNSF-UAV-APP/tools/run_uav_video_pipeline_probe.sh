#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source_file="$repo/NDNSF-UAV-APP/tools/uav_video_pipeline_probe.cpp"
binary="${TMPDIR:-/tmp}/ndnsf-uav-video-pipeline-probe-${UID}"

cxx="${CXX:-c++}"
read -r -a cflags <<<"$(pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)"
read -r -a libs <<<"$(pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)"
"$cxx" -std=c++17 -O2 "${cflags[@]}" "$source_file" "${libs[@]}" -o "$binary"
"$binary"
