#!/bin/bash
# Materialize Core and both native bindings together with the exact host NDN
# runtime libraries used to compile them.  The SIF base image contains a
# different NDN-SVS/NDN-CXX ABI even when the headers have the same digest;
# mixing those DSOs causes free()/invalid-pointer during SVSPubSub startup.
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=40G
#SBATCH --gres=gpu:rtx_5000:1
#SBATCH --time=00:30:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec162/materialization-fix025/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec162/materialization-fix025/slurm-%j.err

set -Eeuo pipefail
umask 077
readonly BASE_SIF='/project/tma1/ndnsf-di/releases/spec162-t009-fix019-core-bindings/runtime.sif'
readonly CORE_LIB='/project/tma1/ndnsf-di/sources/spec162-t009-fix025-core/libndn-service-framework.so'
readonly DI_PYBIND='/project/tma1/ndnsf-di/sources/spec162-t009-fix018-binding/_ndnsf.cpython-310-x86_64-linux-gnu.so'
readonly REPO_PYBIND='/project/tma1/ndnsf-di/sources/spec162-t009-fix019-repo-binding/_py_repoclient.cpython-310-x86_64-linux-gnu.so'
readonly ABI_DIR='/project/tma1/ndnsf-di/sources/spec162-t009-fix020-runtime-abi'
readonly RELEASE_DIR='/project/tma1/ndnsf-di/releases/spec162-t009-fix025-core-bindings-abi'
readonly EVIDENCE_PARENT='/project/tma1/ndnsf-di/evidence/spec162/materialization-fix025'
readonly CORE_SHA256='ed4317d1ff9278ae61b95c104ea1f060f4dcbe1c9c39b6910966c1254283c99c'
readonly DI_PYBIND_SHA256='ded48e939d26500094580cd02f184abe3b070fe96cb155ebcece9c3648993ca1'
readonly REPO_PYBIND_SHA256='5db29dde679b991740b302f3238e8bcd26b9eecf528a8267490c6528531589fc'
readonly BASE_SIF_SHA256='c030edff2d2e4b8b71031385fce34f1c608cf92768b646ab64c484dbeb9d2b4b'
readonly LIBNDN_SVS_SHA256='2f258b7c41f357bb3754bc896e569aaac2744d0a307ab0d2787c8b95cd9bf211'
readonly LIBNDN_CXX_SHA256='cdb79d9f282b7c8528bf2660d58ab896fce6c2c14a51eb9415ec4440cfcc8531'
readonly LIBNAC_ABE_SHA256='1ca3f33ce934e84df5cf6ce2497c42a68999fba39a22f5f6383abb04f482cd05'
readonly LIBNDNSD_SHA256='2fba7b1159e7aa84aeb369943f4ef81977a9ef5356c69cd032500b9f87f4c64f'
readonly LIBOPENABE_SHA256='69a37331b240ffa91b3886cbd2747f72593c7ac3976a0db6456629222c731f10'
readonly LIBRELIC_SHA256='017f36478eb7d5b5830d925caf8041b16cc1e84b2239abb624dd01120f60ac17'
readonly LIBRELIC_EC_SHA256='953169815b3dc2f3d6686483c74cd7a94f8985751076ba1030f58f7c191790c2'
: "${SLURM_JOB_ID:?}"
partial="${EVIDENCE_PARENT}/.fix025-${SLURM_JOB_ID}.partial"
final="${EVIDENCE_PARENT}/fix025-${SLURM_JOB_ID}"
test ! -e "$partial"; test ! -e "$final"; test ! -e "$RELEASE_DIR"; mkdir -p "$partial"
state=FAIL; scratch=''
finish() {
  rc=$?; trap - EXIT INT TERM
  test -z "$scratch" || test ! -d "$scratch" || rm -rf -- "$scratch"
  python3 - "$partial/result.json" "$state" "$rc" "$SLURM_JOB_ID" <<'PY' || true
import json, sys
path, state, rc, job = sys.argv[1:]
with open(path, 'w', encoding='utf-8') as f:
    json.dump({'schemaVersion':'spec162-core-bindings-abi-materialization-v1',
               'state':state,'exitCode':int(rc),'jobId':job}, f, indent=2, sort_keys=True); f.write('\n')
PY
  if test "$state" = PASS && test "$rc" -eq 0; then mv "$partial" "$final"; fi
  exit "$rc"
}
trap finish EXIT INT TERM
test -f "$BASE_SIF"; test -f "$CORE_LIB"; test -f "$DI_PYBIND"; test -f "$REPO_PYBIND"
for f in libndn-svs.so.0.1.0 libndn-cxx.so.0.9.0 libnac-abe.so libndnsd.so.0.1.0 libopenabe.so librelic.so librelic_ec.so; do test -f "$ABI_DIR/$f"; done
printf '%s  %s\n' "$BASE_SIF_SHA256" "$BASE_SIF" | sha256sum -c - > "$partial/base-sif.sha256.log"
printf '%s  %s\n' "$CORE_SHA256" "$CORE_LIB" | sha256sum -c - > "$partial/core-lib.sha256.log"
printf '%s  %s\n' "$DI_PYBIND_SHA256" "$DI_PYBIND" | sha256sum -c - > "$partial/di-pybind.sha256.log"
printf '%s  %s\n' "$REPO_PYBIND_SHA256" "$REPO_PYBIND" | sha256sum -c - > "$partial/repo-pybind.sha256.log"
for spec in \
  "libndn-svs.so.0.1.0:$LIBNDN_SVS_SHA256" \
  "libndn-cxx.so.0.9.0:$LIBNDN_CXX_SHA256" \
  "libnac-abe.so:$LIBNAC_ABE_SHA256" \
  "libndnsd.so.0.1.0:$LIBNDNSD_SHA256" \
  "libopenabe.so:$LIBOPENABE_SHA256" \
  "librelic.so:$LIBRELIC_SHA256" \
  "librelic_ec.so:$LIBRELIC_EC_SHA256"; do
  file=${spec%%:*}; sha=${spec#*:}
  printf '%s  %s\n' "$sha" "$ABI_DIR/$file" | sha256sum -c - >> "$partial/runtime-abi.sha256.log"
done
base="${SLURM_TMPDIR:-/tmp/$USER}"; case "$(realpath -m "$base")" in /home|/home/*|/project|/project/*) base="/tmp/$USER";; esac
mkdir -p "$base"; test "$(df -Pk "$base" | awk 'NR==2 {print $4}')" -ge 26214400
scratch=$(mktemp -d "$base/spec162-core-bindings-abi-${SLURM_JOB_ID}.XXXXXX")
export APPTAINER_CACHEDIR="$scratch/cache" APPTAINER_TMPDIR="$scratch/tmp" TMPDIR="$scratch/tmp"
mkdir -p "$APPTAINER_CACHEDIR" "$APPTAINER_TMPDIR" "$scratch/home"
cat > "$scratch/overlay.def" <<EOF
Bootstrap: localimage
From: $BASE_SIF
%files
    $CORE_LIB /opt/ndnsf-app/lib/libndn-service-framework.so.0.1.0
    $DI_PYBIND /opt/ndnsf-app/python/ndnsf/_ndnsf.cpython-310-x86_64-linux-gnu.so
    $REPO_PYBIND /opt/ndnsf-app/python/py_repoclient/_py_repoclient.cpython-310-x86_64-linux-gnu.so
    $ABI_DIR/libndn-svs.so.0.1.0 /opt/ndnsf-app/lib/libndn-svs.so.0.1.0
    $ABI_DIR/libndn-cxx.so.0.9.0 /opt/ndnsf-app/lib/libndn-cxx.so.0.9.0
    $ABI_DIR/libnac-abe.so /opt/ndnsf-app/lib/libnac-abe.so
    $ABI_DIR/libndnsd.so.0.1.0 /opt/ndnsf-app/lib/libndnsd.so.0.1.0
    $ABI_DIR/libopenabe.so /opt/ndnsf-app/lib/libopenabe.so
    $ABI_DIR/librelic.so /opt/ndnsf-app/lib/librelic.so
    $ABI_DIR/librelic_ec.so /opt/ndnsf-app/lib/librelic_ec.so
%post
    ln -sfn libndn-service-framework.so.0.1.0 /opt/ndnsf-app/lib/libndn-service-framework.so
%labels
    org.ndnsf.di.spec162.fix 024-core-bindings-abi
    org.ndnsf.di.core-sha256 $CORE_SHA256
    org.ndnsf.di-pybind-sha256 $DI_PYBIND_SHA256
    org.ndnsf.repo-pybind-sha256 $REPO_PYBIND_SHA256
    org.ndnsf.di.base-sif-sha256 $BASE_SIF_SHA256
    org.ndnsf.runtime.abi-libndn-svs-sha256 $LIBNDN_SVS_SHA256
    org.ndnsf.runtime.abi-libndn-cxx-sha256 $LIBNDN_CXX_SHA256
    org.ndnsf.runtime.abi-libnac-abe-sha256 $LIBNAC_ABE_SHA256
    org.ndnsf.runtime.abi-libndnsd-sha256 $LIBNDNSD_SHA256
%test
    test -f /opt/ndnsf-app/lib/libndn-service-framework.so.0.1.0
    test -f /opt/ndnsf-app/python/ndnsf/_ndnsf.cpython-310-x86_64-linux-gnu.so
    test -f /opt/ndnsf-app/python/py_repoclient/_py_repoclient.cpython-310-x86_64-linux-gnu.so
    test -f /opt/ndnsf-app/lib/libndn-svs.so.0.1.0
    test -f /opt/ndnsf-app/lib/libndn-cxx.so.0.9.0
EOF
{ echo "host=$(hostname)"; echo "jobId=$SLURM_JOB_ID"; apptainer version; df -Pk "$scratch"; } > "$partial/environment.txt"
apptainer build --fakeroot --force "$scratch/runtime.sif" "$scratch/overlay.def" > "$partial/build.log" 2>&1
test -s "$scratch/runtime.sif"; apptainer test "$scratch/runtime.sif" > "$partial/apptainer-test.txt" 2>&1
for path in \
  /opt/ndnsf-app/lib/libndn-service-framework.so.0.1.0 \
  /opt/ndnsf-app/python/ndnsf/_ndnsf.cpython-310-x86_64-linux-gnu.so \
  /opt/ndnsf-app/python/py_repoclient/_py_repoclient.cpython-310-x86_64-linux-gnu.so \
  /opt/ndnsf-app/lib/libndn-svs.so.0.1.0 /opt/ndnsf-app/lib/libndn-cxx.so.0.9.0 \
  /opt/ndnsf-app/lib/libnac-abe.so /opt/ndnsf-app/lib/libndnsd.so.0.1.0 \
  /opt/ndnsf-app/lib/libopenabe.so /opt/ndnsf-app/lib/librelic.so /opt/ndnsf-app/lib/librelic_ec.so; do
  name=$(basename "$path")
  apptainer exec --cleanenv --containall --home "$scratch/home:/home/tma1" "$scratch/runtime.sif" sha256sum "$path" > "$partial/$name.sha256"
done
test "$(cut -d' ' -f1 < "$partial/libndn-service-framework.so.0.1.0.sha256")" = "$CORE_SHA256"
test "$(cut -d' ' -f1 < "$partial/_ndnsf.cpython-310-x86_64-linux-gnu.so.sha256")" = "$DI_PYBIND_SHA256"
test "$(cut -d' ' -f1 < "$partial/_py_repoclient.cpython-310-x86_64-linux-gnu.so.sha256")" = "$REPO_PYBIND_SHA256"
test "$(cut -d' ' -f1 < "$partial/libndn-svs.so.0.1.0.sha256")" = "$LIBNDN_SVS_SHA256"
test "$(cut -d' ' -f1 < "$partial/libndn-cxx.so.0.9.0.sha256")" = "$LIBNDN_CXX_SHA256"
sif_sha=$(sha256sum "$scratch/runtime.sif" | awk '{print $1}')
mkdir "$RELEASE_DIR"; cp "$scratch/runtime.sif" "$RELEASE_DIR/.runtime.sif.${SLURM_JOB_ID}.partial"; chmod 0444 "$RELEASE_DIR/.runtime.sif.${SLURM_JOB_ID}.partial"; sync "$RELEASE_DIR/.runtime.sif.${SLURM_JOB_ID}.partial"; mv "$RELEASE_DIR/.runtime.sif.${SLURM_JOB_ID}.partial" "$RELEASE_DIR/runtime.sif"
python3 - "$RELEASE_DIR/materialization.json" "$sif_sha" "$SLURM_JOB_ID" <<'PY'
import json, sys
path, sha, job = sys.argv[1:]
with open(path, 'x', encoding='utf-8') as f:
    json.dump({'schemaVersion':'spec162-core-bindings-abi-materialization-v1',
               'releaseId':'spec162-t009-fix025-core-bindings-abi',
               'baseSif':'/project/tma1/ndnsf-di/releases/spec162-t009-fix019-core-bindings/runtime.sif',
               'baseSifSha256':'sha256:c030edff2d2e4b8b71031385fce34f1c608cf92768b646ab64c484dbeb9d2b4b',
               'coreLibrarySha256':'sha256:ed4317d1ff9278ae61b95c104ea1f060f4dcbe1c9c39b6910966c1254283c99c',
               'diPythonBindingSha256':'sha256:ded48e939d26500094580cd02f184abe3b070fe96cb155ebcece9c3648993ca1',
               'repoPythonBindingSha256':'sha256:5db29dde679b991740b302f3238e8bcd26b9eecf528a8267490c6528531589fc',
               'runtimeAbi':{
                   'libndn-svs.so.0.1.0':'sha256:2f258b7c41f357bb3754bc896e569aaac2744d0a307ab0d2787c8b95cd9bf211',
                   'libndn-cxx.so.0.9.0':'sha256:cdb79d9f282b7c8528bf2660d58ab896fce6c2c14a51eb9415ec4440cfcc8531',
                   'libnac-abe.so':'sha256:1ca3f33ce934e84df5cf6ce2497c42a68999fba39a22f5f6383abb04f482cd05',
                   'libndnsd.so.0.1.0':'sha256:2fba7b1159e7aa84aeb369943f4ef81977a9ef5356c69cd032500b9f87f4c64f',
                   'libopenabe.so':'sha256:69a37331b240ffa91b3886cbd2747f72593c7ac3976a0db6456629222c731f10',
                   'librelic.so':'sha256:017f36478eb7d5b5830d925caf8041b16cc1e84b2239abb624dd01120f60ac17',
                   'librelic_ec.so':'sha256:953169815b3dc2f3d6686483c74cd7a94f8985751076ba1030f58f7c191790c2'},
               'sifPath':'/project/tma1/ndnsf-di/releases/spec162-t009-fix025-core-bindings-abi/runtime.sif',
               'sifSha256':'sha256:'+sha,'slurmJobId':job}, f, indent=2, sort_keys=True); f.write('\n')
PY
chmod 0444 "$RELEASE_DIR/materialization.json"; cp "$RELEASE_DIR/materialization.json" "$partial/materialization.json"; sha256sum "$RELEASE_DIR/runtime.sif" > "$partial/durable-runtime.sif.sha256"; state=PASS
