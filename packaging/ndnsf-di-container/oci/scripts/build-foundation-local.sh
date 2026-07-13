#!/usr/bin/env bash
set -euo pipefail

repo=$(git rev-parse --show-toplevel)
seal="$repo/.spec110-build"
base=''
tag=''
output="$repo/results/spec110-itiger-qwen-live/foundation-build/local-foundation.json"
push=0

while (($#)); do
  case "$1" in
    --seal) seal=$2; shift 2 ;;
    --base) base=$2; shift 2 ;;
    --tag) tag=$2; shift 2 ;;
    --output) output=$2; shift 2 ;;
    --push) push=1; shift ;;
    *) printf 'unknown argument: %s\n' "$1" >&2; exit 2 ;;
  esac
done

revision=$(git -C "$repo" rev-parse HEAD)
if [[ -z $base ]]; then
  base=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["baseImages"]["foundation"])' \
    "$repo/packaging/ndnsf-di-container/oci/locks/gpu.lock")
fi
if [[ -z $tag ]]; then
  tag="ghcr.io/matianxing1992/ndnsf-di-foundation:spec110-foundation-$revision"
fi
[[ $base =~ ^[^[:space:]]+@sha256:[a-f0-9]{64}$ ]] || {
  echo FOUNDATION_BASE_MUST_BE_DIGEST >&2; exit 3;
}
test -f "$seal/source-seal.json" || {
  echo FOUNDATION_SOURCE_SEAL_MISSING >&2; exit 3;
}
python3 "$repo/packaging/ndnsf-di-container/oci/scripts/prepare-sealed-context.py" verify \
  --workspace "$repo" \
  --lock "$repo/packaging/ndnsf-di-container/oci/locks/gpu.lock" \
  --output "$seal"
python3 "$repo/packaging/ndnsf-di-container/oci/scripts/preflight-gpu-build.py" \
  --workspace "$repo" --seal "$seal" --output /dev/null

context=$(mktemp -d "${TMPDIR:-/tmp}/spec110-foundation-context.XXXXXX")
trap 'rm -rf "$context"' EXIT
mkdir -p "$context/.spec110-build"
cp -a "$seal/." "$context/.spec110-build/"
for path in \
  packaging/ndnsf-di-container/oci/Dockerfile.foundation \
  packaging/ndnsf-di-container/oci/locks/gpu.lock \
  packaging/ndnsf-di-container/oci/scripts/prepare-openabe-relic.py \
  packaging/ndnsf-di-container/oci/scripts/derive-runtime-packages.py; do
  mkdir -p "$context/$(dirname "$path")"
  cp "$repo/$path" "$context/$path"
done

# BuildKit bind mounts need the workspace.  A read-only bind is not sufficient
# because waf writes its build directory; the disposable context carries the
# exact committed tree and keeps the user's checkout untouched.
git -C "$repo" archive "$revision" | tar -x -C "$context"
cp -a "$seal/." "$context/.spec110-build/"

builder_tag="${tag}-builder"
docker build --progress=plain \
  --file "$context/packaging/ndnsf-di-container/oci/Dockerfile.foundation" \
  --target foundation-builder \
  --build-arg "FOUNDATION_BASE_IMAGE=$base" \
  --build-arg "SOURCE_REVISION=$revision" \
  --build-arg DEPENDENCY_SOURCE_MODE=sealed \
  --tag "$builder_tag" "$context"
docker run --rm \
  -e PATH=/opt/ndnsf-di/current/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  -e LD_LIBRARY_PATH=/opt/ndnsf-di/current/lib \
  "$builder_tag" bash -lc \
  'nfd --version && nfdc --version && test -f /opt/ndnsf-di/current/lib/pkgconfig/libndn-service-framework.pc'

docker build --progress=plain \
  --file "$context/packaging/ndnsf-di-container/oci/Dockerfile.foundation" \
  --target foundation \
  --build-arg "FOUNDATION_BASE_IMAGE=$base" \
  --build-arg "SOURCE_REVISION=$revision" \
  --build-arg DEPENDENCY_SOURCE_MODE=sealed \
  --tag "$tag" "$context"

image_id=$(docker image inspect --format '{{.Id}}' "$tag")
digest=''
if ((push)); then
  [[ $tag == *":spec110-foundation-$revision" ]] || {
    echo FOUNDATION_PUSH_TAG_MUST_BIND_SOURCE_REVISION >&2; exit 3;
  }
  docker push "$tag"
  digest=$(docker image inspect --format '{{index .RepoDigests 0}}' "$tag")
fi
mkdir -p "$(dirname "$output")"
python3 - "$output" "$revision" "$base" "$tag" "$image_id" "$digest" <<'PY'
import datetime,json,sys
from pathlib import Path
path,revision,base,tag,image_id,digest=sys.argv[1:]
record={
  'schemaVersion':'spec110-local-foundation-v1',
  'status':'PASS',
  'createdAt':datetime.datetime.now(datetime.timezone.utc).isoformat(),
  'sourceRevision':revision,
  'baseImage':base,
  'localTag':tag,
  'localImageId':image_id,
  'publishedDigest':digest or None,
}
Path(path).write_text(json.dumps(record,indent=2,sort_keys=True)+'\n')
print(json.dumps(record,indent=2,sort_keys=True))
PY
