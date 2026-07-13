#!/bin/sh
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../../../.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/spec110-rootless-build.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
mkdir -p "$tmp/bin" "$tmp/project/campaigns/spec110/rootless-build/probe-001" "$tmp/slurm-tmp" "$tmp/source"

cat >"$tmp/bin/podman" <<'SH'
#!/bin/sh
printf '%s\n' "$*" >>"$PODMAN_CAPTURE"
case "$1" in
  --version) echo 'podman version 5.2.2'; exit 0 ;;
esac
operation=
output=
previous=
for value in "$@"; do
  [ "$previous" = -o ] && output=$value
  [ "$value" = build ] && operation=build
  [ "$value" = save ] && operation=save
  previous=$value
done
[ "$operation" = build ] && exit 0
if [ "$operation" = save ]; then
  python3 - "$output" <<'PY'
import hashlib,json,tarfile,sys,tempfile
from pathlib import Path
out=Path(sys.argv[1]);manifest=b'{"schemaVersion":2,"config":{"mediaType":"application/vnd.oci.image.config.v1+json","digest":"sha256:' + b'0'*64 + b'","size":0},"layers":[]}'
digest=hashlib.sha256(manifest).hexdigest();index=json.dumps({'schemaVersion':2,'manifests':[{'mediaType':'application/vnd.oci.image.manifest.v1+json','digest':'sha256:'+digest,'size':len(manifest)}]},separators=(',',':')).encode()
with tempfile.TemporaryDirectory() as tmp:
 root=Path(tmp);(root/'blobs/sha256').mkdir(parents=True);(root/'index.json').write_bytes(index);(root/'oci-layout').write_text('{"imageLayoutVersion":"1.0.0"}');(root/'blobs/sha256'/digest).write_bytes(manifest)
 with tarfile.open(out,'w') as archive:
  archive.add(root/'index.json',arcname='index.json');archive.add(root/'oci-layout',arcname='oci-layout');archive.add(root/'blobs',arcname='blobs')
PY
  exit 0
fi
exit 2
SH
cat >"$tmp/bin/buildah" <<'SH'
#!/bin/sh
printf '%s\n' "$*" >>"${BUILDAH_CAPTURE:-/dev/null}"
[ "$1" = --version ] && { echo 'buildah version 1.33.7'; exit 0; }
operation=
destination=
for value in "$@"; do
  [ "$value" = info ] && operation=info
  [ "$value" = bud ] && operation=bud
  [ "$value" = push ] && operation=push
  destination=$value
done
if [ "$operation" = info ]; then
  [ "${BUILDAH_FAIL_INFO:-0}" = 1 ] && exit 125
  echo '{"store":{"graphDriverName":"vfs"}}'
  exit 0
fi
if [ "$operation" = bud ]; then
  [ -n "${SPEC110_TEST_EVIDENCE_DIR:-}" ] && mkdir "${SPEC110_TEST_EVIDENCE_DIR}/manifest.json"
  exit 0
fi
if [ "$operation" = push ]; then
  output=${destination#oci-archive:}
  python3 - "$output" <<'PY'
import hashlib,json,tarfile,sys,tempfile
from pathlib import Path
out=Path(sys.argv[1]);manifest=b'{"schemaVersion":2,"config":{"mediaType":"application/vnd.oci.image.config.v1+json","digest":"sha256:' + b'0'*64 + b'","size":0},"layers":[]}'
digest=hashlib.sha256(manifest).hexdigest();index=json.dumps({'schemaVersion':2,'manifests':[{'mediaType':'application/vnd.oci.image.manifest.v1+json','digest':'sha256:'+digest,'size':len(manifest)}]},separators=(',',':')).encode()
with tempfile.TemporaryDirectory() as tmp:
 root=Path(tmp);(root/'blobs/sha256').mkdir(parents=True);(root/'index.json').write_bytes(index);(root/'oci-layout').write_text('{"imageLayoutVersion":"1.0.0"}');(root/'blobs/sha256'/digest).write_bytes(manifest)
 with tarfile.open(out,'w') as archive:
  archive.add(root/'index.json',arcname='index.json');archive.add(root/'oci-layout',arcname='oci-layout');archive.add(root/'blobs',arcname='blobs')
PY
  exit 0
fi
exit 2
SH
cat >"$tmp/bin/apptainer" <<'SH'
#!/bin/sh
case "$1" in
  version) echo 'apptainer version 1.3.4' ;;
  build)
    case "$2" in
      *runtime.sif.partial) [ "${APPTAINER_FAIL_RUNTIME:-0}" = 1 ] && exit 125 ;;
    esac
    printf sif-bytes >"$2"
    ;;
  exec)
    while [ "$#" -gt 0 ] && [ "$1" != buildah ]; do shift; done
    if [ "${1:-}" = buildah ]; then shift; exec buildah "$@"; fi
    exit 0
    ;;
  *) exit 2 ;;
esac
SH
chmod 0755 "$tmp/bin/podman" "$tmp/bin/buildah" "$tmp/bin/apptainer"

PODMAN_CAPTURE="$tmp/podman.log" PATH="$tmp/bin:$PATH" USER=tester SLURM_JOB_ID=701 \
SLURM_TMPDIR="$tmp/slurm-tmp" SLURMD_NODENAME=compute-test \
NDNSF_SPEC110_ALLOW_TEST_ROOT=1 \
"$repo/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/rootless-build.sh" \
  --mode diagnostic --source-root "$tmp/source" --project-root "$tmp/project" \
  --release-id probe-001 \
  --evidence-dir "$tmp/project/campaigns/spec110/rootless-build/probe-001" \
  --probe-base "docker.io/library/alpine@sha256:$(printf '%064d' 0)" \
  --gpu-build-base "example/build@sha256:$(printf '%064d' 1)" \
  --gpu-runtime-base "example/runtime@sha256:$(printf '%064d' 2)"

python3 - "$tmp/project/campaigns/spec110/rootless-build/probe-001/manifest.json" "$tmp/slurm-tmp" <<'PY'
import json,sys
value=json.load(open(sys.argv[1],encoding='utf-8'))
assert value['status']=='PASS',value
assert value['diagnosticOnly'] is True
assert value['slurm']['jobId']=='701'
assert value['storage']['graphroot'].startswith(sys.argv[2]+'/')
assert value['storage']['scratchSource']=='SLURM_TMPDIR'
assert not value['storage']['graphroot'].startswith('/home/')
assert value['artifacts']['ociDigest'].startswith('sha256:')
assert value['artifacts']['sifSha256'].startswith('sha256:')
assert value['physicalProduction']=='DEFERRED'
PY
grep -q -- '--root' "$tmp/podman.log"
grep -q -- '--runroot' "$tmp/podman.log"
[ -f "$tmp/project/campaigns/spec110/rootless-build/probe-001/artifacts/runtime.oci.tar" ]
[ -f "$tmp/project/campaigns/spec110/rootless-build/probe-001/artifacts/runtime.sif" ]
[ ! -e "$tmp/slurm-tmp/ndnsf-di/701/probe-001" ]

mkdir -p "$tmp/project/campaigns/spec110/rootless-build/probe-002"
PODMAN_CAPTURE="$tmp/podman.log" BUILDAH_CAPTURE="$tmp/buildah.log" PATH="$tmp/bin:$PATH" \
USER=tester SLURM_JOB_ID=702 SLURM_TMPDIR="$tmp/slurm-tmp" SLURMD_NODENAME=compute-test \
NDNSF_SPEC110_ALLOW_TEST_ROOT=1 \
"$repo/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/rootless-build.sh" \
  --mode diagnostic --source-root "$tmp/source" --project-root "$tmp/project" \
  --release-id probe-002 \
  --evidence-dir "$tmp/project/campaigns/spec110/rootless-build/probe-002" \
  --builder-mode apptainer-sif \
  --builder-oci "quay.io/buildah/stable@sha256:$(printf '%064d' 3)" \
  --probe-base "docker.io/library/alpine@sha256:$(printf '%064d' 0)" \
  --gpu-build-base "example/build@sha256:$(printf '%064d' 1)" \
  --gpu-runtime-base "example/runtime@sha256:$(printf '%064d' 2)"

python3 - "$tmp/project/campaigns/spec110/rootless-build/probe-002/manifest.json" <<'PY'
import json,sys
value=json.load(open(sys.argv[1],encoding='utf-8'))
assert value['status']=='PASS',value
assert value['builder']['requestedMode']=='apptainer-sif'
assert value['builder']['effectiveMode']=='apptainer-sif'
assert value['builder']['storageDriver']=='vfs'
assert value['builder']['isolation']=='chroot'
assert value['builder']['namespaceMode']=='apptainer-fakeroot'
assert value['builder']['sifSha256'].startswith('sha256:')
assert value['versions']['podman']==''
PY
grep -q -- '--storage-driver vfs info' "$tmp/buildah.log"
grep -q -- 'bud --isolation chroot' "$tmp/buildah.log"
grep -q -- 'push localhost/spec110/probe-002:sealed oci-archive:' "$tmp/buildah.log"
[ ! -e "$tmp/slurm-tmp/ndnsf-di/702/probe-002" ]

mkdir -p "$tmp/project/campaigns/spec110/rootless-build/probe-userns-fail"
set +e
BUILDAH_FAIL_INFO=1 BUILDAH_CAPTURE="$tmp/buildah-fail.log" PATH="$tmp/bin:$PATH" \
USER=tester SLURM_JOB_ID=703 SLURM_TMPDIR="$tmp/slurm-tmp" SLURMD_NODENAME=compute-test \
NDNSF_SPEC110_ALLOW_TEST_ROOT=1 \
"$repo/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/rootless-build.sh" \
  --mode diagnostic --source-root "$tmp/source" --project-root "$tmp/project" \
  --release-id probe-userns-fail \
  --evidence-dir "$tmp/project/campaigns/spec110/rootless-build/probe-userns-fail" \
  --builder-mode apptainer-sif \
  --builder-oci "quay.io/buildah/stable@sha256:$(printf '%064d' 3)" \
  --probe-base "docker.io/library/alpine@sha256:$(printf '%064d' 0)" \
  --gpu-build-base "example/build@sha256:$(printf '%064d' 1)" \
  --gpu-runtime-base "example/runtime@sha256:$(printf '%064d' 2)"
userns_status=$?
set -e
[ "$userns_status" -eq 5 ]
python3 - "$tmp/project/campaigns/spec110/rootless-build/probe-userns-fail/manifest.json" <<'PY'
import json,sys
value=json.load(open(sys.argv[1],encoding='utf-8'))
assert value['status']=='FAIL'
assert value['reasonCode']=='ROOTLESS_BUILD_USER_NAMESPACE_UNAVAILABLE'
assert value['scratchRetained'] is False
PY
[ ! -e "$tmp/slurm-tmp/ndnsf-di/703/probe-userns-fail" ]

mkdir -p "$tmp/project/campaigns/spec110/rootless-build/probe-partial-fail"
set +e
APPTAINER_FAIL_RUNTIME=1 BUILDAH_CAPTURE="$tmp/buildah-partial.log" PATH="$tmp/bin:$PATH" \
USER=tester SLURM_JOB_ID=704 SLURM_TMPDIR="$tmp/slurm-tmp" SLURMD_NODENAME=compute-test \
NDNSF_SPEC110_ALLOW_TEST_ROOT=1 \
"$repo/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/rootless-build.sh" \
  --mode diagnostic --source-root "$tmp/source" --project-root "$tmp/project" \
  --release-id probe-partial-fail \
  --evidence-dir "$tmp/project/campaigns/spec110/rootless-build/probe-partial-fail" \
  --builder-mode apptainer-sif \
  --builder-oci "quay.io/buildah/stable@sha256:$(printf '%064d' 3)" \
  --probe-base "docker.io/library/alpine@sha256:$(printf '%064d' 0)" \
  --gpu-build-base "example/build@sha256:$(printf '%064d' 1)" \
  --gpu-runtime-base "example/runtime@sha256:$(printf '%064d' 2)"
partial_status=$?
set -e
[ "$partial_status" -eq 5 ]
[ ! -e "$tmp/project/campaigns/spec110/rootless-build/probe-partial-fail/artifacts.partial.704" ]
[ ! -e "$tmp/slurm-tmp/ndnsf-di/704/probe-partial-fail" ]

evidence_fail="$tmp/project/campaigns/spec110/rootless-build/probe-evidence-fail"
mkdir -p "$evidence_fail"
set +e
SPEC110_TEST_EVIDENCE_DIR="$evidence_fail" BUILDAH_CAPTURE="$tmp/buildah-evidence.log" \
PATH="$tmp/bin:$PATH" USER=tester SLURM_JOB_ID=705 SLURM_TMPDIR="$tmp/slurm-tmp" \
SLURMD_NODENAME=compute-test NDNSF_SPEC110_ALLOW_TEST_ROOT=1 \
"$repo/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/rootless-build.sh" \
  --mode diagnostic --source-root "$tmp/source" --project-root "$tmp/project" \
  --release-id probe-evidence-fail --evidence-dir "$evidence_fail" \
  --builder-mode apptainer-sif \
  --builder-oci "quay.io/buildah/stable@sha256:$(printf '%064d' 3)" \
  --probe-base "docker.io/library/alpine@sha256:$(printf '%064d' 0)" \
  --gpu-build-base "example/build@sha256:$(printf '%064d' 1)" \
  --gpu-runtime-base "example/runtime@sha256:$(printf '%064d' 2)" \
  >"$tmp/evidence-fail.out" 2>"$tmp/evidence-fail.err"
evidence_status=$?
set -e
[ "$evidence_status" -eq 6 ]
grep -q ROOTLESS_BUILD_EVIDENCE_WRITE_FAILED "$tmp/evidence-fail.err"
[ ! -e "$evidence_fail/artifacts" ]
[ ! -e "$tmp/slurm-tmp/ndnsf-di/705/probe-evidence-fail" ]

mkdir -p "$tmp/seal/workspace" "$tmp/seal/dependencies/dep"
for directory in "$tmp/seal/workspace" "$tmp/seal/dependencies/dep"; do
  git -C "$directory" init -q
  git -C "$directory" config user.email test@example.invalid
  git -C "$directory" config user.name test
  printf '%s\n' source >"$directory/source.txt"
  git -C "$directory" add source.txt
  git -C "$directory" commit -qm initial
done
revision=$(git -C "$tmp/seal/dependencies/dep" rev-parse HEAD)
cat >"$tmp/seal/lock.json" <<EOF
{"schemaVersion":"ndnsf-di-gpu-lock-v1","sourceRepositories":{"dep":{"revision":"$revision"}}}
EOF
seal="$repo/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/seal-rootless-source.py"
python3 "$seal" create --source-root "$tmp/seal" --lock "$tmp/seal/lock.json"
python3 "$seal" verify --source-root "$tmp/seal" --lock "$tmp/seal/lock.json"
printf '%s\n' changed >"$tmp/seal/dependencies/dep/source.txt"
if python3 "$seal" verify --source-root "$tmp/seal" --lock "$tmp/seal/lock.json"; then
  echo SOURCE_SEAL_DIRTY_UNEXPECTED_PASS >&2; exit 1
fi

echo ROOTLESS_BUILD_PIPELINE_PASS
