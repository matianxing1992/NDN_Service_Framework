#!/bin/sh
set -eu

usage() {
  echo "usage: $0 --rootfs DIRECTORY --tag TAG --build-base NAME@sha256:HEX --runtime-base NAME@sha256:HEX [--release-id ID] [--source-revision REV] [--check-only]" >&2
  exit 2
}
rootfs=
tag=
build_base=
runtime_base=
release_id=unknown
source_revision=unknown
check_only=false
while [ "$#" -gt 0 ]; do
  case "$1" in
    --rootfs) rootfs=$2; shift 2 ;;
    --tag) tag=$2; shift 2 ;;
    --build-base) build_base=$2; shift 2 ;;
    --runtime-base) runtime_base=$2; shift 2 ;;
    --release-id) release_id=$2; shift 2 ;;
    --source-revision) source_revision=$2; shift 2 ;;
    --check-only) check_only=true; shift ;;
    *) usage ;;
  esac
done
if [ -z "$rootfs" ] || [ -z "$tag" ] || [ -z "$build_base" ] || [ -z "$runtime_base" ]; then usage; fi
digest_pattern='^.+@sha256:[a-f0-9]{64}$'
printf '%s\n' "$build_base" | grep -Eq "$digest_pattern" || { echo "OCI_BUILD_BASE_NOT_DIGEST_PINNED" >&2; exit 4; }
printf '%s\n' "$runtime_base" | grep -Eq "$digest_pattern" || { echo "OCI_RUNTIME_BASE_NOT_DIGEST_PINNED" >&2; exit 4; }
[ -f "$rootfs/manifest/SHA256SUMS" ] || { echo "OCI_ROOTFS_MANIFEST_MISSING:$rootfs" >&2; exit 4; }
repo=$(cd -P -- "$(dirname -- "$0")/../../../.." && pwd)
"$repo/packaging/ndnsf-di-container/oci/scripts/scan-release.sh" --path "$rootfs" >/dev/null
case "$rootfs" in
  "$repo"/*) release_root=${rootfs#"$repo"/} ;;
  *) echo "OCI_ROOTFS_OUTSIDE_BUILD_CONTEXT:$rootfs" >&2; exit 4 ;;
esac
[ "$check_only" = false ] || { echo "OCI_BUILD_INPUT_PASS releaseRoot=$release_root"; exit 0; }
exec docker build --file "$repo/packaging/ndnsf-di-container/oci/Dockerfile.cpu" \
  --build-arg "BUILD_BASE_IMAGE=$build_base" \
  --build-arg "RUNTIME_BASE_IMAGE=$runtime_base" \
  --build-arg "RELEASE_ROOT=$release_root" \
  --build-arg "RELEASE_ID=$release_id" \
  --build-arg "SOURCE_REVISION=$source_revision" \
  --tag "$tag" "$repo"
