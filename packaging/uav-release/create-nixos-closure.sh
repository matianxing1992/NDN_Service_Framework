#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
release_tarball="${1:?usage: $0 <release-tarball> [output-dir]}"
output_dir="${2:-$repo_root/RELEASE}"
mkdir -p "$output_dir"
output_dir="$(cd "$output_dir" && pwd)"

if [[ "$release_tarball" != /* ]]; then
  release_tarball="$(cd "$(dirname "$release_tarball")" && pwd)/$(basename "$release_tarball")"
fi

if [[ ! -f "$release_tarball" ]]; then
  echo "missing release tarball: $release_tarball" >&2
  exit 1
fi

if [[ -e /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh ]]; then
  # shellcheck disable=SC1091
  . /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh
fi

if ! command -v nix-build >/dev/null 2>&1 || ! command -v nix-store >/dev/null 2>&1; then
  echo "nix-build and nix-store are required" >&2
  exit 1
fi

arch="$(uname -m)"
case "$arch" in
  x86_64) target_arch="x86_64" ;;
  aarch64|arm64) target_arch="aarch64" ;;
  *) target_arch="$arch" ;;
esac

version="${NDNSF_UAV_RELEASE_VERSION:-}"
release_stem="NDNSF-UAV-nixos-$target_arch"
if [[ -n "$version" ]]; then
  release_stem="$release_stem-$version"
fi
result_link="$output_dir/$release_stem-result"
paths_file="$output_dir/$release_stem.paths"
closure="$output_dir/$release_stem.closure"
closure_gz="$closure.gz"

nix-build "$repo_root/packaging/uav-release/nixos-binary/default.nix" \
  --arg releaseTarball "$release_tarball" \
  --argstr releaseName ndnsf-uav-release \
  -o "$result_link"

out="$(readlink -f "$result_link")"
echo "NIX_OUT=$out"

"$out/scripts/check-runtime-deps.sh" || true
nix-store -qR "$out" > "$paths_file"
nix-store --export $(cat "$paths_file") > "$closure"
gzip -f "$closure"

echo "created $result_link -> $out"
echo "created $paths_file"
echo "created $closure_gz"
