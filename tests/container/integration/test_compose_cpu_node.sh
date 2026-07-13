#!/bin/sh
set -eu

[ "${NDNSF_CONTAINER_LIVE:-0}" = 1 ] || {
  echo "SKIP: set NDNSF_CONTAINER_LIVE=1 for Docker integration" >&2
  exit 77
}
[ -n "${NDNSF_OCI_IMAGE:-}" ] || { echo "NDNSF_OCI_IMAGE digest reference is required" >&2; exit 2; }

repo=$(cd -P -- "$(dirname -- "$0")/../../.." && pwd)
run_id=${NDNSF_RUN_ID:-spec108-compose-$$}
root=${NDNSF_TEST_ROOT:-/tmp/$run_id}
[ ! -e "$root" ] || { echo "test root exists: $root" >&2; exit 2; }
mkdir -p "$root/identity" "$root/project" "$root/nfd-run" "$root/evidence"
chmod 700 "$root/identity"
chmod 770 "$root/nfd-run"

export COMPOSE_PROJECT_NAME="$run_id"
export NDNSF_IDENTITY_ROOT="$root/identity"
export NDNSF_PROJECT_ROOT="$root/project"
export NDNSF_NFD_RUN_DIR="$root/nfd-run"
export NDNSF_EVIDENCE_ROOT="$root/evidence"
compose="$repo/packaging/ndnsf-di-container/adapters/docker-compose/compose.yaml"

cleanup() { docker compose -f "$compose" stop >/dev/null 2>&1 || true; }
trap cleanup EXIT INT TERM
docker info >/dev/null
docker compose -f "$compose" config --quiet
docker compose -f "$compose" up -d --wait
test -S "$root/nfd-run/nfd.sock"
docker compose -f "$compose" ps --format json > "$root/evidence/compose-ps-before.json"
docker compose -f "$compose" stop
docker compose -f "$compose" up -d --wait
test -S "$root/nfd-run/nfd.sock"
docker compose -f "$compose" ps --format json > "$root/evidence/compose-ps-after.json"
test -d "$root/project"
