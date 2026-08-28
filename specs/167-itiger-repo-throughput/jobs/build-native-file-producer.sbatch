#!/bin/bash
# Build only the Python 3.10 native binding for the persistent-file producer.
# The Qwen model, CUDA foundation, and existing SIF are inputs, not rebuilds.
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=24G
#SBATCH --time=00:30:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec167/native-build/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec167/native-build/slurm-%j.err

set -Eeuo pipefail
umask 077

readonly BASE_SIF="${SPEC167_NATIVE_BASE_SIF:?}"
readonly SOURCE_ROOT="${SPEC167_NATIVE_SOURCE_ROOT:?}"
readonly OUTPUT_ROOT="${SPEC167_NATIVE_OUTPUT_ROOT:?}"
readonly SOURCE_SHA256="${SPEC167_NATIVE_SOURCE_SHA256:?}"
readonly EVIDENCE_ROOT="/project/tma1/ndnsf-di/evidence/spec167/native-build"
readonly SOURCE_CPP="$SOURCE_ROOT/src/ndnsf/_ndnsf.cpp"
readonly OUTPUT_SO="$OUTPUT_ROOT/_ndnsf.cpython-310-x86_64-linux-gnu.so"

: "${SLURM_JOB_ID:?}"
partial="$EVIDENCE_ROOT/.native-file-producer-${SLURM_JOB_ID}.partial"
final="$EVIDENCE_ROOT/native-file-producer-${SLURM_JOB_ID}"
test ! -e "$partial"
test ! -e "$final"
test ! -e "$OUTPUT_ROOT"
mkdir -p "$partial" "$OUTPUT_ROOT"

scratch=''
state=FAIL
finish() {
  rc=$?
  trap - EXIT INT TERM
  if test -n "$scratch" && test -d "$scratch"; then
    rm -rf -- "$scratch"
  fi
  python3 - "$partial/result.json" "$state" "$rc" "$SLURM_JOB_ID" <<'PY' || true
import json
import sys

path, state, rc, job = sys.argv[1:]
with open(path, "w", encoding="utf-8") as stream:
    json.dump({
        "schemaVersion": "spec167-native-file-producer-build-v1",
        "state": state,
        "exitCode": int(rc),
        "jobId": job,
    }, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY
  if test "$state" = PASS && test "$rc" -eq 0; then
    mv "$partial" "$final"
  fi
  exit "$rc"
}
trap finish EXIT INT TERM

test -f "$BASE_SIF"
test -f "$SOURCE_CPP"
printf '%s  %s\n' "$SOURCE_SHA256" "$SOURCE_CPP" |
  sha256sum -c - > "$partial/source-cpp.sha256"

base="${SLURM_TMPDIR:-/tmp/$USER}"
case "$(realpath -m "$base")" in
  /home|/home/*|/project|/project/*) base="/tmp/$USER" ;;
esac
mkdir -p "$base"
scratch=$(mktemp -d "$base/spec167-native-file-producer-${SLURM_JOB_ID}.XXXXXX")
mkdir -p "$scratch/include/ndn-base" "$scratch/include/ndnsf-app" \
  "$scratch/include/python3.10" "$scratch/include/boost" "$scratch/lib/ndn-base" \
  "$scratch/lib/ndnsf-app" "$scratch/lib/boost" "$scratch/lib/system" \
  "$scratch/build" "$scratch/test/ndnsf"

{
  echo "jobId=$SLURM_JOB_ID"
  echo "host=$(hostname)"
  echo "baseSif=$BASE_SIF"
  echo "sourceRoot=$SOURCE_ROOT"
  echo "sourceSha256=$SOURCE_SHA256"
  echo "g++=$(g++ --version | head -1)"
  echo "apptainer=$(apptainer version)"
  df -Pk "$scratch"
} > "$partial/environment.txt"

# The runtime SIF contains the exact NDN/NDNSF headers, Python 3.10 headers,
# and shared libraries used by the accepted Qwen runtime.  The host compiler
# is used only because the sealed runtime intentionally has no compiler.
apptainer exec --cleanenv --no-mount all "$BASE_SIF" /usr/bin/tar -cf - \
  -C /opt/ndn-base/include . |
  tar -xf - -C "$scratch/include/ndn-base"
apptainer exec --cleanenv --no-mount all "$BASE_SIF" /usr/bin/tar -cf - \
  -C /opt/ndnsf-app/include . |
  tar -xf - -C "$scratch/include/ndnsf-app"
apptainer exec --cleanenv --no-mount all "$BASE_SIF" /usr/bin/tar -cf - \
  -C /usr/local/include/python3.10 . |
  tar -xf - -C "$scratch/include/python3.10"
mapfile -t boost_names < <(
  apptainer exec --cleanenv --no-mount all "$BASE_SIF" /usr/bin/find \
    /usr/lib/x86_64-linux-gnu -maxdepth 1 -type f -name 'libboost_*.so*' \
    -printf '%f\n'
)
test "${#boost_names[@]}" -gt 0
apptainer exec --cleanenv --no-mount all "$BASE_SIF" /usr/bin/tar -cf - \
  -C /usr/lib/x86_64-linux-gnu "${boost_names[@]}" |
  tar -xf - -C "$scratch/lib/boost"
cp -a "$SOURCE_ROOT/boost/." "$scratch/include/boost/"

apptainer exec --cleanenv --no-mount all "$BASE_SIF" /usr/bin/tar -cf - \
  -C /opt/ndn-base/lib . |
  tar -xf - -C "$scratch/lib/ndn-base"
apptainer exec --cleanenv --no-mount all "$BASE_SIF" /usr/bin/tar -cf - \
  -C /opt/ndnsf-app/lib . |
  tar -xf - -C "$scratch/lib/ndnsf-app"
apptainer exec --cleanenv --no-mount all "$BASE_SIF" /usr/bin/tar -cf - \
  -C /usr/lib/x86_64-linux-gnu libcrypto.so.1.1 libssl.so.1.1 |
  tar -xf - -C "$scratch/lib/system"

g++ -std=c++17 -O2 -fPIC -c "$SOURCE_CPP" \
  -I"$SOURCE_ROOT" \
  -I"$SOURCE_ROOT/ndn-service-framework" \
  -I"$scratch/include/ndn-base" \
  -I"$scratch/include/ndnsf-app" \
  -I"$scratch/include/python3.10" \
  -I"$SOURCE_ROOT/pybind11/include" \
  -I"$scratch/include/boost" \
  -o "$scratch/build/_ndnsf.o"

g++ -shared "$scratch/build/_ndnsf.o" \
  -L"$scratch/lib/ndnsf-app" -L"$scratch/lib/ndn-base" \
  -L"$scratch/lib/boost" -L"$scratch/lib/system" -L/usr/lib/x86_64-linux-gnu \
  -Wl,--disable-new-dtags -Wl,-rpath,/opt/ndnsf-app/lib \
  -Wl,-rpath,/opt/ndn-base/lib \
  -lndn-service-framework -lndn-svs -lnac-abe -lndn-cxx \
  -l:libboost_chrono.so.1.71.0 -l:libboost_date_time.so.1.71.0 \
  -l:libboost_log.so.1.71.0 -l:libboost_thread.so.1.71.0 \
  -l:libboost_stacktrace_backtrace.so.1.71.0 -l:libcrypto.so.1.1 \
  -l:libssl.so.1.1 -lsqlite3 -lrt \
  -lpthread -lndnsd \
  -o "$OUTPUT_SO"

cp -a "$OUTPUT_SO" "$scratch/test/ndnsf/"
touch "$scratch/test/ndnsf/__init__.py"
apptainer exec --cleanenv --no-mount all \
  --bind "$scratch/test:/native-test:ro" "$BASE_SIF" \
  /opt/venv/bin/python -c \
  'import sys; sys.path.insert(0, "/native-test"); import ndnsf._ndnsf as m; print(m.FileSegmentedObjectProducer.__name__)' \
  > "$partial/import-probe.txt"

sha256sum "$OUTPUT_SO" > "$partial/extension.sha256"
stat -c '%n %s bytes' "$OUTPUT_SO" > "$partial/extension-size.txt"
cp "$partial/extension.sha256" "$OUTPUT_ROOT/extension.sha256"
cat > "$OUTPUT_ROOT/manifest.json" <<EOF
{
  "schemaVersion": "spec167-native-file-producer-v1",
  "baseSif": "$BASE_SIF",
  "sourceCppSha256": "sha256:$SOURCE_SHA256",
  "extension": "$OUTPUT_SO",
  "extensionSha256": "sha256:$(cut -d' ' -f1 "$partial/extension.sha256")",
  "pythonAbi": "cpython-310-x86_64-linux-gnu",
  "persistentFileStream": true
}
EOF
chmod 0444 "$OUTPUT_SO" "$OUTPUT_ROOT/extension.sha256" "$OUTPUT_ROOT/manifest.json"
state=PASS
