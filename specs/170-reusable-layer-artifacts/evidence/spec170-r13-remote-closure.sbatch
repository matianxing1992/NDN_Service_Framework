#!/usr/bin/env bash
# Compute-node promotion parity gate for the exact locally built r13 SIF.
# Tiger stages, hashes, and executes the image; it never rebuilds it.
#SBATCH --job-name=spec170-r13-closure
#SBATCH --partition=bigTiger
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --time=00:20:00
#SBATCH --exclude=itiger07
#SBATCH --output=/project/tma1/ndnsf-di/releases/spec170-runtime-31d9f547-post-selection-v11-20260817/evidence/remote-closure-%j.out

set -euo pipefail
unset APPTAINERENV_HOME APPTAINERENV_PWD SINGULARITYENV_HOME SINGULARITYENV_PWD

release=/project/tma1/ndnsf-di/releases/spec170-runtime-31d9f547-post-selection-v11-20260817
source_sif="$release/runtime.sif"
expected_sha=4e29b0fdc14eba8172fb927dcea07676e807f1b577ea76a82b786b407cbc2840
expected_apptainer=1.5.3
scratch="/tmp/ndnsf-di-${SLURM_JOB_ID}"
evidence="$release/evidence/remote-closure-${SLURM_JOB_ID}"
cpu_bundle="$release/network-bundle-cpu"
gpu_bundle="$release/network-bundle-gpu"
lock="$release/evidence/spec170-local-candidate-31d9f547-r13-library-lock-v2.json"
validator="$release/operator-tools/validate-sif-library-closure.py"
bundle_validator="$release/operator-tools/validate-spec170-network-bundle.py"

mkdir -p "$scratch" "$evidence"
trap 'rm -rf "$scratch"' EXIT

echo "R13_CLOSURE_STEP=compute-contract node=$(hostname -s)"
command -v apptainer | tee "$evidence/apptainer-path.txt"
apptainer version | tee "$evidence/apptainer-version.txt"
rpm -q apptainer | tee "$evidence/apptainer-package.txt"
actual_apptainer=$(apptainer version | sed -E 's/[^0-9]*([0-9]+\.[0-9]+\.[0-9]+).*/\1/')
test "$actual_apptainer" = "$expected_apptainer" || {
  echo "R13_APPTAINER_VERSION_MISMATCH expected=$expected_apptainer actual=$actual_apptainer" >&2
  exit 4
}

echo R13_CLOSURE_STEP=project-hash
test "$(sha256sum "$source_sif" | cut -d' ' -f1)" = "$expected_sha"
echo R13_CLOSURE_STEP=stage
cp --reflink=auto "$source_sif" "$scratch/runtime.sif"
test "$(sha256sum "$scratch/runtime.sif" | cut -d' ' -f1)" = "$expected_sha"
printf '%s  %s\n' "$expected_sha" "$source_sif" > "$evidence/project-sif.sha256"
printf '%s  %s\n' "$expected_sha" "$scratch/runtime.sif" > "$evidence/staged-sif.sha256"

echo R13_CLOSURE_STEP=labels
apptainer inspect --json "$scratch/runtime.sif" > "$evidence/sif-inspect.json"
python3 - "$evidence/sif-inspect.json" <<'PY'
import json
import sys

labels = json.load(open(sys.argv[1], encoding="utf-8"))["data"]["attributes"]["labels"]
assert labels["org.ndnsf.di.release"] == "spec170-runtime-31d9f547-post-selection-v11-20260817"
assert labels["org.ndnsf.di.tiger-action"] == "verify-hash-and-execute-only"
PY

echo R13_CLOSURE_STEP=python-ort
apptainer exec --cleanenv "$scratch/runtime.sif" /opt/venv/bin/python -c \
  'import ndnsf._ndnsf as native; print(native.__file__)' \
  > "$evidence/python-import.txt"
apptainer exec --cleanenv "$scratch/runtime.sif" /opt/venv/bin/python -c \
  'import onnxruntime as ort; print(ort.__version__); print(ort.get_available_providers())' \
  > "$evidence/ort-providers.txt"
test "$(apptainer exec --cleanenv "$scratch/runtime.sif" sh -lc \
  'find /opt/venv/lib/python3.10/site-packages/ndnsf -maxdepth 1 -type f -name "_ndnsf*.so" | wc -l')" = 1

echo R13_CLOSURE_STEP=provider-wiring
apptainer exec --cleanenv --bind "$cpu_bundle:/bundle:ro" --pwd /bundle \
  "$scratch/runtime.sif" /opt/ndnsf-di/current/bin/di-native-provider \
  --plan /bundle/native-execution-plan.json \
  --manifest /bundle/service-manifest.json \
  --service /Inference/NativeTracer \
  --provider /NDNSF-DI/Tracer/provider/preflight \
  --roles all --workers 1 \
  --group /NDNSF-DI/Tracer/group \
  --controller /NDNSF-DI/Tracer/controller \
  --trust-schema /bundle/trust-schema.conf \
  --artifact-cache-dir "$scratch/provider-cache" \
  --check-only --wiring-check-only \
  > "$evidence/provider-wiring-check.txt" 2>&1
grep -q 'NDNSF_DI_NATIVE_PROVIDER_CHECK_OK' "$evidence/provider-wiring-check.txt"

echo R13_CLOSURE_STEP=library-closure
python3 "$validator" \
  --sif "$scratch/runtime.sif" \
  --provider /opt/ndnsf-di/current/bin/di-native-provider \
  --extension /opt/venv/lib/python3.10/site-packages/ndnsf/_ndnsf.cpython-310-x86_64-linux-gnu.so \
  --library-root /opt/ndnsf-di/current/lib \
  --library-root /opt/onnxruntime/lib \
  --lock "$lock" \
  --output "$evidence/sif-library-closure.json"

echo R13_CLOSURE_STEP=bundle-contracts
python3 "$bundle_validator" \
  --bundle "$cpu_bundle" \
  --baseline-bundle "$gpu_bundle" \
  --workload "$cpu_bundle/spec170-d0-current-sif-workload.sh" \
  --allow-diff service-manifest.json \
  --expected-execution-provider cpu \
  --output "$evidence/cpu-bundle-preflight.json"
python3 "$bundle_validator" \
  --bundle "$gpu_bundle" \
  --workload "$gpu_bundle/spec170-d1-current-sif-workload.sh" \
  --single-provider \
  --expected-execution-provider cuda \
  --output "$evidence/gpu-bundle-preflight.json"

printf 'SPEC170_R13_REMOTE_CLOSURE_PASS jobId=%s node=%s sif=sha256:%s\n' \
  "$SLURM_JOB_ID" "$(hostname -s)" "$expected_sha"
