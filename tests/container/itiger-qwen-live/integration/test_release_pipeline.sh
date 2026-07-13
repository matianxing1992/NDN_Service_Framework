#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/spec110-release.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM

dockerfile="$repo/packaging/ndnsf-di-container/oci/Dockerfile.gpu"
ignore="$repo/packaging/ndnsf-di-container/oci/.dockerignore"
probe="$repo/packaging/ndnsf-di-container/oci/scripts/probe-runtime.py"
workflow="$repo/.github/workflows/ndnsf-di-itiger-image.yml"
grep -q 'sourceRepositories' "$dockerfile"
grep -q 'USER 65532:65532' "$dockerfile"
grep -q 'HEALTHCHECK' "$dockerfile"
grep -q 'daemon-policy="allocation-scoped-only"' "$dockerfile"
grep -q '^identities$' "$ignore"
grep -q '^results$' "$ignore"
grep -q '^models$' "$ignore"
grep -q 'CUDAExecutionProvider' "$probe"
grep -q 'FAIL_ORT_CPU_FALLBACK' "$repo/packaging/ndnsf-di-container/lib/gpu_compatibility.py"
grep -q 'workflow_dispatch:' "$workflow"
grep -q 'GPU_BUILD_BASE_IMAGE=' "$workflow"
grep -q 'provenance: mode=max' "$workflow"
grep -q 'cosign sign --yes --bundle' "$workflow"
grep -q 'anchore/sbom-action' "$workflow"
grep -q 'scan-secrets.py' "$workflow"
grep -q 'release.create_immutable_gpu_release_record' "$workflow"
if grep -q 'CANDIDATE_ID' "$workflow"; then
  echo RUNTIME_RELEASE_MUST_NOT_CIRCULARLY_BIND_CANDIDATE >&2; exit 1
fi

mkdir -p "$tmp/source" "$tmp/release-evidence"
printf '%s\n' clean >"$tmp/source/input.txt"
printf '%s\n' clean >"$tmp/release-evidence/manifest.json"
python3 "$repo/packaging/ndnsf-di-container/oci/scripts/scan-secrets.py" \
  --path "$tmp/source" --scope source --output "$tmp/source-scan.json"
python3 "$repo/packaging/ndnsf-di-container/oci/scripts/scan-secrets.py" \
  --path "$tmp/release-evidence" --scope release-evidence \
  --output "$tmp/release-evidence-scan.json"

python3 - "$repo" "$tmp" <<'PY'
import copy,hashlib,json,sys
from pathlib import Path
repo=Path(sys.argv[1]);root=Path(sys.argv[2]);sys.path.insert(0,str(repo/'packaging/ndnsf-di-container/lib'))
import release
d='sha256:'+'a'*64
manifest={
 'schemaVersion':'1.0','releaseId':'spec110-r1',
 'sourceRevision':'8b9a4fe709d35b9e4d4961eaa25cefad45cfc0b2','createdAt':'2026-07-13T18:00:00Z',
 'images':{'linux-amd64-gpu':{'reference':'ghcr.io/example/ndnsf-di@'+d,'digest':d,'platform':'linux/amd64','backend':'onnxruntime-cuda'}},
 'buildInputs':[],'sbom':{'location':'sbom.spdx.json','digest':d},
 'provenance':{'builder':'github-actions','digest':d},'compatibility':{'architecture':'amd64','cuda':'12.4'}}
manifest_path=root/'release-manifest.json';manifest_path.write_text(json.dumps(manifest))
signature={'schemaVersion':'cosign-verification-bundle-v1','verified':True,'imageDigest':d,
 'issuer':'https://token.actions.githubusercontent.com','subject':'repo:example/ndnsf:ref:refs/heads/main',
 'transparencyLog':'rekor-entry-1','visibility':'private','authMode':'ghcr-token'}
signature_path=root/'signature.json';signature_path.write_text(json.dumps(signature))
record=release.create_immutable_gpu_release_record(manifest_path=manifest_path,signature_bundle_path=signature_path,output_path=root/'runtime-release.json')
release.validate_immutable_gpu_release_record(record)
try:release.create_immutable_gpu_release_record(manifest_path=manifest_path,signature_bundle_path=signature_path,output_path=root/'runtime-release.json')
except release.ReleaseError as error:assert str(error)=='SPEC110_RELEASE_RECORD_EXISTS'
else:raise AssertionError('release overwrite accepted')
tampered=copy.deepcopy(record);tampered['visibility']='public'
try:release.validate_immutable_gpu_release_record(tampered)
except release.ReleaseError as error:assert str(error)=='SPEC110_RELEASE_RECORD_TAMPERED'
else:raise AssertionError('release tamper accepted')
PY

mkdir -p "$tmp/clean" "$tmp/bad"
printf '%s\n' clean >"$tmp/clean/manifest.txt"
printf '%s\n' 'Authorization: Bearer synthetic-secret' >"$tmp/bad/evidence.log"
python3 "$repo/packaging/ndnsf-di-container/oci/scripts/scan-secrets.py" --path "$tmp/clean" --scope artifact --output "$tmp/clean-scan.json"
if python3 "$repo/packaging/ndnsf-di-container/oci/scripts/scan-secrets.py" --path "$tmp/bad" --scope log --output "$tmp/bad-scan.json"; then
  echo SECRET_SCAN_NEGATIVE_UNEXPECTED_PASS >&2; exit 1
fi

mkdir -p "$tmp/bin"
cat >"$tmp/bin/apptainer" <<'SH'
#!/bin/sh
case "$1" in
 version) echo 'apptainer version 1.3.3' ;;
 build) printf sif-bytes >"$2"; [ "${APPTAINER_FAKE_FAIL:-0}" = 0 ] || exit 9 ;;
 exec) printf '%s\n' "$@" >"$CAPTURE" ;;
 *) exit 2 ;;
esac
SH
chmod 0755 "$tmp/bin/apptainer"
materialize="$repo/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/materialize-sif.sh"
oci="ghcr.io/example/ndnsf-di@sha256:$(printf '%064d' 0 | tr 0 a)"
PATH="$tmp/bin:$PATH" "$materialize" --oci-reference "$oci" --sif "$tmp/runtime.sif" --record "$tmp/materialization.json"
PATH="$tmp/bin:$PATH" "$materialize" --oci-reference "$oci" --sif "$tmp/runtime.sif" --record "$tmp/materialization.json" | grep -q MATERIALIZATION_EXISTING_VERIFIED
if APPTAINER_FAKE_FAIL=1 PATH="$tmp/bin:$PATH" "$materialize" --oci-reference "$oci" --sif "$tmp/failed.sif" --record "$tmp/failed.json"; then
  echo MATERIALIZATION_FAILURE_UNEXPECTED_PASS >&2; exit 1
fi
[ ! -e "$tmp/failed.sif.partial" ]
cp "$tmp/runtime.sif" "$tmp/tampered.sif"
cp "$tmp/materialization.json" "$tmp/tampered.json"
printf x >>"$tmp/tampered.sif"
if PATH="$tmp/bin:$PATH" "$materialize" --oci-reference "$oci" --sif "$tmp/tampered.sif" --record "$tmp/tampered.json"; then
  echo MATERIALIZATION_TAMPER_UNEXPECTED_PASS >&2; exit 1
fi

project="$tmp/project/ndnsf-di"; scratch="$tmp/scratch/77"
mkdir -p "$project/releases" "$project/models" "$project/identities/provider" "$project/evidence" "$scratch"
sif_sha=sha256:$(sha256sum "$tmp/runtime.sif" | cut -d' ' -f1)
CAPTURE="$tmp/apptainer-args" NDNSF_SPEC110_ALLOW_TEST_ROOT=1 SLURM_JOB_ID=77 PATH="$tmp/bin:$PATH" \
  "$repo/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-container.sh" \
  --sif "$tmp/runtime.sif" --sif-sha256 "$sif_sha" --project "$project" \
  --scratch "$scratch" --identity "$project/identities/provider" -- /bin/true
grep -q '^--containall$' "$tmp/apptainer-args"
grep -q '^--no-home$' "$tmp/apptainer-args"
grep -q "$project/releases:/release:ro" "$tmp/apptainer-args"
grep -q "$project/models:/models:ro" "$tmp/apptainer-args"
grep -q "$project/identities/provider:/identity:ro" "$tmp/apptainer-args"
grep -q "$project/evidence:/evidence:rw" "$tmp/apptainer-args"
if grep -q "$project:/project:rw" "$tmp/apptainer-args"; then
  echo BROAD_PROJECT_WRITE_BIND_FOUND >&2; exit 1
fi

echo RELEASE_PIPELINE_PASS
