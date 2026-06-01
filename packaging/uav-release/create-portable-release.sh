#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
version="${NDNSF_UAV_RELEASE_VERSION:-$(date +%Y%m%d_%H%M%S)}"
output_dir="${1:-$repo_root/release-artifacts}"
mkdir -p "$output_dir"
output_dir="$(cd "$output_dir" && pwd)"
release_name="NDNSF-UAV-ubuntu20-x86_64-$version"
stage="$output_dir/$release_name"

include_system_libs="${NDNSF_UAV_RELEASE_INCLUDE_SYSTEM_LIBS:-0}"

usage() {
  cat <<USAGE
Usage: $0 [output-dir]

Build a portable NDNSF-UAV release directory and tarball for Ubuntu 20.04 x86_64.

Environment:
  NDNSF_UAV_RELEASE_VERSION=<name>          Override release suffix.
  NDNSF_UAV_RELEASE_INCLUDE_SYSTEM_LIBS=1   Also bundle most ldd-discovered
                                            Ubuntu runtime libraries. This is
                                            larger, but useful for same-OS
                                            lab machines with sparse packages.

The bundle always includes NDNSF, ndn-cxx, ndn-svs, NDNSD, NAC-ABE, OpenABE,
and RELIC dynamic libraries when they are visible through ldd.
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

rm -rf "$stage"
mkdir -p "$stage"/{bin,lib,config,certs,scripts,share/ndnsf-uav/videos}

required_bins=(
  "$repo_root/build/examples/App_ServiceController"
  "$repo_root/build/examples/UavGroundStationApp"
  "$repo_root/build/examples/UavDroneApp"
)

for binary in "${required_bins[@]}"; do
  if [[ ! -x "$binary" ]]; then
    echo "missing built binary: $binary" >&2
    echo "run ./waf build first" >&2
    exit 1
  fi
  cp -a "$binary" "$stage/bin/"
done

cp -a "$repo_root/NDNSF-UAV-APP/configs/"*.conf "$stage/config/"
cp -a "$repo_root/NDNSF-UAV-APP/configs/"*.policies "$stage/config/" 2>/dev/null || true
cp -a "$repo_root/NDNSF-UAV-APP/videos/drone.mp4" "$stage/share/ndnsf-uav/videos/" 2>/dev/null || true

is_required_private_lib() {
  local path="$1"
  local base
  base="$(basename "$path")"
  case "$base" in
    libndn-service-framework.so*|libndn-cxx.so*|libndn-svs.so*|libndnsd.so*|\
    libnac-abe.so*|libopenabe.so*|librelic.so*|librelic_ec.so*)
      return 0
      ;;
  esac
  return 1
}

is_never_bundle_lib() {
  local path="$1"
  local base
  base="$(basename "$path")"
  case "$base" in
    linux-vdso.so*|ld-linux*.so*|libc.so*|libpthread.so*|libdl.so*|\
    librt.so*|libm.so*|libresolv.so*|libnsl.so*|libutil.so*)
      return 0
      ;;
  esac
  return 1
}

copy_lib() {
  local lib="$1"
  [[ -f "$lib" || -L "$lib" ]] || return 0
  local base
  base="$(basename "$lib")"
  cp -aL "$lib" "$stage/lib/$base"
  if [[ -L "$lib" ]]; then
    local real
    real="$(readlink -f "$lib")"
    [[ -f "$real" ]] && cp -aL "$real" "$stage/lib/$(basename "$real")"
  fi
}

collect_libraries() {
  local binary="$1"
  ldd "$binary" | awk '
    /=>/ && $3 ~ /^\// { print $3 }
    /^[[:space:]]*\// { print $1 }
  '
}

declare -A seen_libs=()
queue=()
for binary in "${required_bins[@]}"; do
  while IFS= read -r lib; do
    [[ -n "$lib" ]] && queue+=("$lib")
  done < <(collect_libraries "$binary")
done

while ((${#queue[@]})); do
  lib="${queue[0]}"
  queue=("${queue[@]:1}")
  real="$(readlink -f "$lib" 2>/dev/null || true)"
  [[ -n "$real" && -e "$real" ]] || continue
  [[ -n "${seen_libs[$real]:-}" ]] && continue
  seen_libs[$real]=1

  if is_never_bundle_lib "$real"; then
    continue
  fi

  if is_required_private_lib "$real" || [[ "$include_system_libs" == "1" ]]; then
    copy_lib "$real"
    while IFS= read -r dep; do
      [[ -n "$dep" ]] && queue+=("$dep")
    done < <(collect_libraries "$real" 2>/dev/null || true)
  fi
done

set_bundle_runpath() {
  local exe="$1"
  if command -v patchelf >/dev/null 2>&1; then
    patchelf --set-rpath '$ORIGIN/../lib' "$exe"
  fi
}

for exe in "$stage/bin/App_ServiceController" \
           "$stage/bin/UavGroundStationApp" \
           "$stage/bin/UavDroneApp"; do
  set_bundle_runpath "$exe"
done

cat > "$stage/bin/ndnsf-uav-controller" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$here/bin/App_ServiceController" "$@"
EOF

cat > "$stage/bin/ndnsf-uav-gs" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$here/bin/UavGroundStationApp" "$@"
EOF

cat > "$stage/bin/ndnsf-uav-drone" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$here/bin/UavDroneApp" "$@"
EOF

chmod +x "$stage/bin/"*

cat > "$stage/scripts/check-runtime-deps.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

echo "==> Checking bundled executables"
status=0
for exe in "$here"/bin/App_ServiceController \
           "$here"/bin/UavGroundStationApp \
           "$here"/bin/UavDroneApp; do
  echo "-- $(basename "$exe")"
  readelf -d "$exe" | grep -E 'RPATH|RUNPATH' || true
  ldd "$exe" | grep -E 'lib(ndn-cxx|ndn-service-framework|ndn-svs|ndnsd|nac-abe|openabe|relic)' || true
  if ldd "$exe" | grep -q "not found"; then
    ldd "$exe" | grep "not found" || true
    status=1
  else
    echo "   all dynamic libraries resolved"
  fi
done

echo "==> Checking NFD socket availability"
if [[ -S /run/nfd/nfd.sock || -S /var/run/nfd/nfd.sock ]]; then
  echo "   NFD Unix socket found"
else
  echo "   NFD Unix socket not found; start NFD before running NDNSF-UAV apps"
fi

exit "$status"
EOF
chmod +x "$stage/scripts/check-runtime-deps.sh"

cat > "$stage/README_RELEASE.md" <<EOF
# NDNSF-UAV Portable Release

Built on: $(lsb_release -ds 2>/dev/null || true)

This bundle is intended for Ubuntu 20.04 x86_64 machines. It bundles the
NDNSF runtime libraries and the NDN libraries used by the apps, including
ndn-cxx when it is visible through ldd. NFD is not bundled; run NFD on each
machine and configure faces/routes/certificates normally.

Run a quick dependency check:

\`\`\`bash
./scripts/check-runtime-deps.sh
\`\`\`

Example commands:

\`\`\`bash
./bin/ndnsf-uav-controller --controller-prefix /example/uav/controller --policy-file config/uav_demo.policies

./bin/ndnsf-uav-gs --runtime-config config/uav_runtime.conf --app-config config/ground-station.conf

./bin/ndnsf-uav-drone --runtime-config config/uav_runtime.conf --app-config config/drone-A.conf --drone-id A
\`\`\`

If patchelf is available on the build host, the binaries are patched with:

\`\`\`text
\$ORIGIN/../lib
\`\`\`

The wrapper commands still set LD_LIBRARY_PATH as a fallback and to support
hosts where patchelf is not installed.

For same-OS lab machines with very sparse packages, rebuild this tarball with:

\`\`\`bash
NDNSF_UAV_RELEASE_INCLUDE_SYSTEM_LIBS=1 packaging/uav-release/create-portable-release.sh
\`\`\`

That larger bundle still does not include NFD, kernel drivers, camera devices,
PX4/jMAVSim, or certificates.
EOF

tarball="$output_dir/$release_name.tar.gz"
(cd "$output_dir" && tar -czf "$tarball" "$release_name")

echo "created $stage"
echo "created $tarball"
echo "run: $stage/scripts/check-runtime-deps.sh"
