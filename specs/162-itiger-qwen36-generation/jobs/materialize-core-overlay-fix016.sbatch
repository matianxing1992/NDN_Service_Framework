#!/bin/bash
# Build a new immutable SIF with the verified Core library in the image.
# This is not a mounted runtime override and never mutates the base release.
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=22G
#SBATCH --gres=gpu:rtx_5000:1
#SBATCH --time=00:30:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec162/materialization-fix016/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec162/materialization-fix016/slurm-%j.err

set -Eeuo pipefail
umask 077

readonly BASE_SIF='/project/tma1/ndnsf-di/releases/spec162-t009-6a0fd98f4c19-a001/runtime.sif'
readonly CORE_LIB='/project/tma1/ndnsf-di/sources/spec162-t009-fix016-core/libndn-service-framework.so'
readonly RELEASE_ID='spec162-t009-fix016-core-scopekeys'
readonly RELEASE_DIR="/project/tma1/ndnsf-di/releases/${RELEASE_ID}"
readonly EVIDENCE_PARENT='/project/tma1/ndnsf-di/evidence/spec162/materialization-fix016'
readonly CORE_SHA256='cf5aaaa2052330e995ae8ea19a4e68fe51ad0e610e4a248cca4865e99be4aada'
readonly BASE_SIF_SHA256='5bf682b3b7178e88a91d977dbabdf88c2f032aa3ddc984adcdd357e4b3b5f0d5'

: "${SLURM_JOB_ID:?SLURM_JOB_ID_REQUIRED}"
partial="${EVIDENCE_PARENT}/.fix016-${SLURM_JOB_ID}.partial"
final="${EVIDENCE_PARENT}/fix016-${SLURM_JOB_ID}"
test ! -e "$partial"
test ! -e "$final"
test ! -e "$RELEASE_DIR"
mkdir -p "$partial"

terminal_state=FAIL
scratch=''
finish() {
  rc=$?
  trap - EXIT INT TERM
  test -z "$scratch" || test ! -d "$scratch" || rm -rf -- "$scratch"
  python3 - "$partial/result.json" "$terminal_state" "$rc" "$SLURM_JOB_ID" <<'PY' || true
import json
import sys
path, state, rc, job = sys.argv[1:]
with open(path, 'w', encoding='utf-8') as stream:
    json.dump({'schemaVersion': 'spec162-core-overlay-materialization-v1',
               'state': state, 'exitCode': int(rc), 'jobId': job},
              stream, indent=2, sort_keys=True)
    stream.write('\n')
PY
  if test "$terminal_state" = PASS && test "$rc" -eq 0; then
    mv "$partial" "$final"
  fi
  exit "$rc"
}
trap finish EXIT INT TERM

test -f "$BASE_SIF"
test -f "$CORE_LIB"
printf '%s  %s\n' "$BASE_SIF_SHA256" "$BASE_SIF" | sha256sum -c - > "$partial/base-sif.sha256.log"
printf '%s  %s\n' "$CORE_SHA256" "$CORE_LIB" | sha256sum -c - > "$partial/core-lib.sha256.log"

base="${SLURM_TMPDIR:-/tmp/$USER}"
case "$(realpath -m "$base")" in
  /home|/home/*|/project|/project/*) base="/tmp/$USER" ;;
esac
mkdir -p "$base"
test "$(df -Pk "$base" | awk 'NR==2 {print $4}')" -ge 26214400
scratch=$(mktemp -d "$base/spec162-core-overlay-${SLURM_JOB_ID}.XXXXXX")
export APPTAINER_CACHEDIR="$scratch/cache"
export APPTAINER_TMPDIR="$scratch/tmp"
export TMPDIR="$scratch/tmp"
mkdir -p "$APPTAINER_CACHEDIR" "$APPTAINER_TMPDIR" "$scratch/home"

cat > "$scratch/overlay.def" <<EOF
Bootstrap: localimage
From: $BASE_SIF

%files
    $CORE_LIB /opt/ndnsf-app/lib/libndn-service-framework.so.0.1.0

%post
    ln -sfn libndn-service-framework.so.0.1.0 /opt/ndnsf-app/lib/libndn-service-framework.so

%labels
    org.ndnsf.di.spec162.fix 016-core-collaboration-scope-keys
    org.ndnsf.di.native-core-sha256 $CORE_SHA256
    org.ndnsf.di.base-sif-sha256 $BASE_SIF_SHA256

%test
    test -f /opt/ndnsf-app/lib/libndn-service-framework.so.0.1.0
EOF

{
  echo "host=$(hostname)"
  echo "jobId=$SLURM_JOB_ID"
  echo "baseSif=$BASE_SIF"
  echo "baseSifSha256=sha256:$BASE_SIF_SHA256"
  echo "coreLibrary=$CORE_LIB"
  echo "coreLibrarySha256=sha256:$CORE_SHA256"
  apptainer version
  df -Pk "$scratch"
} > "$partial/environment.txt"

apptainer build --fakeroot --force "$scratch/runtime.sif" "$scratch/overlay.def" > "$partial/build.log" 2>&1
test -s "$scratch/runtime.sif"
apptainer test "$scratch/runtime.sif" > "$partial/apptainer-test.txt" 2>&1
apptainer exec --cleanenv --containall --home "$scratch/home:/home/tma1" "$scratch/runtime.sif" sha256sum /opt/ndnsf-app/lib/libndn-service-framework.so.0.1.0 > "$partial/runtime-core-probe.txt"
test "$(cut -d' ' -f1 < "$partial/runtime-core-probe.txt")" = "$CORE_SHA256"

sif_sha=$(sha256sum "$scratch/runtime.sif" | awk '{print $1}')
mkdir "$RELEASE_DIR"
cp "$scratch/runtime.sif" "$RELEASE_DIR/.runtime.sif.${SLURM_JOB_ID}.partial"
chmod 0444 "$RELEASE_DIR/.runtime.sif.${SLURM_JOB_ID}.partial"
sync "$RELEASE_DIR/.runtime.sif.${SLURM_JOB_ID}.partial"
mv "$RELEASE_DIR/.runtime.sif.${SLURM_JOB_ID}.partial" "$RELEASE_DIR/runtime.sif"
python3 - "$RELEASE_DIR/materialization.json" "$sif_sha" "$SLURM_JOB_ID" <<'PY'
import json
import sys
path, sif_sha, job = sys.argv[1:]
with open(path, 'x', encoding='utf-8') as stream:
    json.dump({'schemaVersion': 'spec162-core-overlay-materialization-v1',
               'releaseId': 'spec162-t009-fix016-core-scopekeys',
               'baseSif': '/project/tma1/ndnsf-di/releases/spec162-t009-6a0fd98f4c19-a001/runtime.sif',
               'baseSifSha256': 'sha256:5bf682b3b7178e88a91d977dbabdf88c2f032aa3ddc984adcdd357e4b3b5f0d5',
               'coreLibrarySha256': 'sha256:cf5aaaa2052330e995ae8ea19a4e68fe51ad0e610e4a248cca4865e99be4aada',
               'sifPath': '/project/tma1/ndnsf-di/releases/spec162-t009-fix016-core-scopekeys/runtime.sif',
               'sifSha256': 'sha256:' + sif_sha,
               'slurmJobId': job}, stream, indent=2, sort_keys=True)
    stream.write('\n')
PY
chmod 0444 "$RELEASE_DIR/materialization.json"
cp "$RELEASE_DIR/materialization.json" "$partial/materialization.json"
sha256sum "$RELEASE_DIR/runtime.sif" > "$partial/durable-runtime.sif.sha256"
terminal_state=PASS
