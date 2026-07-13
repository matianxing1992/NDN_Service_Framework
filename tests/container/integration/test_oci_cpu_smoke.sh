#!/bin/sh
set -eu

[ "${NDNSF_CONTAINER_LIVE:-0}" = 1 ] || {
  echo "SKIP: set NDNSF_CONTAINER_LIVE=1 for Docker integration" >&2
  exit 77
}
[ -n "${NDNSF_CONTAINER_RELEASE_ROOT:-}" ] || {
  echo "NDNSF_CONTAINER_RELEASE_ROOT is required" >&2
  exit 2
}
[ -n "${NDNSF_BUILD_BASE_IMAGE:-}" ] || { echo "NDNSF_BUILD_BASE_IMAGE digest reference is required" >&2; exit 2; }
[ -n "${NDNSF_RUNTIME_BASE_IMAGE:-}" ] || { echo "NDNSF_RUNTIME_BASE_IMAGE digest reference is required" >&2; exit 2; }

repo=$(cd -P -- "$(dirname -- "$0")/../../.." && pwd)
run_id=${NDNSF_RUN_ID:-spec108-oci-smoke-$$}
image="ndnsf-di-spec108:$run_id"
rootfs="$repo/dist/ndnsf-di-container/$run_id/rootfs"
[ ! -e "$rootfs" ] || { echo "rootfs exists: $rootfs" >&2; exit 2; }
mkdir -p "$(dirname -- "$rootfs")"

docker info >/dev/null
"$repo/packaging/ndnsf-di-container/oci/scripts/prepare-rootfs.sh" \
  --release "$NDNSF_CONTAINER_RELEASE_ROOT" --output "$rootfs"
"$repo/packaging/ndnsf-di-container/oci/scripts/build-image.sh" \
  --rootfs "$rootfs" --tag "$image" \
  --build-base "$NDNSF_BUILD_BASE_IMAGE" --runtime-base "$NDNSF_RUNTIME_BASE_IMAGE" \
  --release-id "$run_id" --source-revision "$(git -C "$repo" rev-parse HEAD)"
docker run --rm --entrypoint /usr/local/bin/ndnsf-di-container-entrypoint "$image" exec /bin/true
docker image inspect "$image" --format '{{.Id}}'
