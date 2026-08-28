#!/bin/bash
# Build a coherent SIF containing the fix-016 Core and matching Python binding.
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=22G
#SBATCH --gres=gpu:rtx_5000:1
#SBATCH --time=00:30:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec162/materialization-fix017/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec162/materialization-fix017/slurm-%j.err

set -Eeuo pipefail
umask 077
readonly BASE_SIF='/project/tma1/ndnsf-di/releases/spec162-t009-fix016-core-scopekeys/runtime.sif'
readonly CORE_LIB='/project/tma1/ndnsf-di/sources/spec162-t009-fix016-core/libndn-service-framework.so'
readonly PYBIND='/project/tma1/ndnsf-di/sources/spec162-t009-fix017-binding/_ndnsf.cpython-310-x86_64-linux-gnu.so'
readonly RELEASE_DIR='/project/tma1/ndnsf-di/releases/spec162-t009-fix017-core-binding'
readonly EVIDENCE_PARENT='/project/tma1/ndnsf-di/evidence/spec162/materialization-fix017'
readonly CORE_SHA256='cf5aaaa2052330e995ae8ea19a4e68fe51ad0e610e4a248cca4865e99be4aada'
readonly PYBIND_SHA256='55b177a0a1bfb4cb9ded87b4652cbd5aadd13061c849eec772eb984c16a76b86'
readonly BASE_SIF_SHA256='689e45d81150de465d7251f96c29cf5fb3f18fff4ae644c21c23a583ed3cf96c'
: "${SLURM_JOB_ID:?}"
partial="${EVIDENCE_PARENT}/.fix017-${SLURM_JOB_ID}.partial"
final="${EVIDENCE_PARENT}/fix017-${SLURM_JOB_ID}"
test ! -e "$partial"; test ! -e "$final"; test ! -e "$RELEASE_DIR"; mkdir -p "$partial"
state=FAIL; scratch=''
finish() {
  rc=$?; trap - EXIT INT TERM
  test -z "$scratch" || test ! -d "$scratch" || rm -rf -- "$scratch"
  python3 - "$partial/result.json" "$state" "$rc" "$SLURM_JOB_ID" <<'PY' || true
import json, sys
path, state, rc, job = sys.argv[1:]
with open(path, 'w', encoding='utf-8') as f:
    json.dump({'schemaVersion':'spec162-core-binding-materialization-v1',
               'state':state,'exitCode':int(rc),'jobId':job}, f, indent=2, sort_keys=True); f.write('\n')
PY
  if test "$state" = PASS && test "$rc" -eq 0; then mv "$partial" "$final"; fi
  exit "$rc"
}
trap finish EXIT INT TERM
test -f "$BASE_SIF"; test -f "$CORE_LIB"; test -f "$PYBIND"
printf '%s  %s\n' "$BASE_SIF_SHA256" "$BASE_SIF" | sha256sum -c - > "$partial/base-sif.sha256.log"
printf '%s  %s\n' "$CORE_SHA256" "$CORE_LIB" | sha256sum -c - > "$partial/core-lib.sha256.log"
printf '%s  %s\n' "$PYBIND_SHA256" "$PYBIND" | sha256sum -c - > "$partial/pybind.sha256.log"
base="${SLURM_TMPDIR:-/tmp/$USER}"; case "$(realpath -m "$base")" in /home|/home/*|/project|/project/*) base="/tmp/$USER";; esac
mkdir -p "$base"; test "$(df -Pk "$base" | awk 'NR==2 {print $4}')" -ge 26214400
scratch=$(mktemp -d "$base/spec162-core-binding-${SLURM_JOB_ID}.XXXXXX")
export APPTAINER_CACHEDIR="$scratch/cache" APPTAINER_TMPDIR="$scratch/tmp" TMPDIR="$scratch/tmp"
mkdir -p "$APPTAINER_CACHEDIR" "$APPTAINER_TMPDIR" "$scratch/home"
cat > "$scratch/overlay.def" <<EOF
Bootstrap: localimage
From: $BASE_SIF
%files
    $CORE_LIB /opt/ndnsf-app/lib/libndn-service-framework.so.0.1.0
    $PYBIND /opt/ndnsf-app/python/ndnsf/_ndnsf.cpython-310-x86_64-linux-gnu.so
%post
    ln -sfn libndn-service-framework.so.0.1.0 /opt/ndnsf-app/lib/libndn-service-framework.so
%labels
    org.ndnsf.di.spec162.fix 017-core-binding
    org.ndnsf.di.core-sha256 $CORE_SHA256
    org.ndnsf.di.pybind-sha256 $PYBIND_SHA256
    org.ndnsf.di.base-sif-sha256 $BASE_SIF_SHA256
%test
    test -f /opt/ndnsf-app/lib/libndn-service-framework.so.0.1.0
    test -f /opt/ndnsf-app/python/ndnsf/_ndnsf.cpython-310-x86_64-linux-gnu.so
EOF
{ echo "host=$(hostname)"; echo "jobId=$SLURM_JOB_ID"; apptainer version; df -Pk "$scratch"; } > "$partial/environment.txt"
apptainer build --fakeroot --force "$scratch/runtime.sif" "$scratch/overlay.def" > "$partial/build.log" 2>&1
test -s "$scratch/runtime.sif"; apptainer test "$scratch/runtime.sif" > "$partial/apptainer-test.txt" 2>&1
apptainer exec --cleanenv --containall --home "$scratch/home:/home/tma1" "$scratch/runtime.sif" sha256sum /opt/ndnsf-app/lib/libndn-service-framework.so.0.1.0 > "$partial/core-probe.txt"
apptainer exec --cleanenv --containall --home "$scratch/home:/home/tma1" "$scratch/runtime.sif" sha256sum /opt/ndnsf-app/python/ndnsf/_ndnsf.cpython-310-x86_64-linux-gnu.so > "$partial/pybind-probe.txt"
test "$(cut -d' ' -f1 < "$partial/core-probe.txt")" = "$CORE_SHA256"
test "$(cut -d' ' -f1 < "$partial/pybind-probe.txt")" = "$PYBIND_SHA256"
sif_sha=$(sha256sum "$scratch/runtime.sif" | awk '{print $1}')
mkdir "$RELEASE_DIR"; cp "$scratch/runtime.sif" "$RELEASE_DIR/.runtime.sif.${SLURM_JOB_ID}.partial"; chmod 0444 "$RELEASE_DIR/.runtime.sif.${SLURM_JOB_ID}.partial"; sync "$RELEASE_DIR/.runtime.sif.${SLURM_JOB_ID}.partial"; mv "$RELEASE_DIR/.runtime.sif.${SLURM_JOB_ID}.partial" "$RELEASE_DIR/runtime.sif"
python3 - "$RELEASE_DIR/materialization.json" "$sif_sha" "$SLURM_JOB_ID" <<'PY'
import json, sys
path, sha, job = sys.argv[1:]
with open(path, 'x', encoding='utf-8') as f:
    json.dump({'schemaVersion':'spec162-core-binding-materialization-v1',
               'releaseId':'spec162-t009-fix017-core-binding',
               'baseSif':'/project/tma1/ndnsf-di/releases/spec162-t009-fix016-core-scopekeys/runtime.sif',
               'baseSifSha256':'sha256:689e45d81150de465d7251f96c29cf5fb3f18fff4ae644c21c23a583ed3cf96c',
               'coreLibrarySha256':'sha256:cf5aaaa2052330e995ae8ea19a4e68fe51ad0e610e4a248cca4865e99be4aada',
               'pythonBindingSha256':'sha256:55b177a0a1bfb4cb9ded87b4652cbd5aadd13061c849eec772eb984c16a76b86',
               'sifPath':'/project/tma1/ndnsf-di/releases/spec162-t009-fix017-core-binding/runtime.sif',
               'sifSha256':'sha256:'+sha,'slurmJobId':job}, f, indent=2, sort_keys=True); f.write('\n')
PY
chmod 0444 "$RELEASE_DIR/materialization.json"; cp "$RELEASE_DIR/materialization.json" "$partial/materialization.json"; sha256sum "$RELEASE_DIR/runtime.sif" > "$partial/durable-runtime.sif.sha256"; state=PASS
