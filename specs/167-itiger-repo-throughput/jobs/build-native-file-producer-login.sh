#!/bin/bash
# Login-node fallback for clusters whose compute image has no compiler.
# This still compiles against headers/libraries extracted from the immutable
# runtime SIF; it does not rebuild the foundation, CUDA layers, or models.
set -Eeuo pipefail
umask 077

: "${SPEC167_NATIVE_BASE_SIF:?}"
: "${SPEC167_NATIVE_SOURCE_ROOT:?}"
: "${SPEC167_NATIVE_OUTPUT_ROOT:?}"
: "${SPEC167_NATIVE_SOURCE_SHA256:?}"

base_sif="$SPEC167_NATIVE_BASE_SIF"
source_root="$SPEC167_NATIVE_SOURCE_ROOT"
output_root="$SPEC167_NATIVE_OUTPUT_ROOT"
source_cpp="$source_root/src/ndnsf/_ndnsf.cpp"
output_so="$output_root/_ndnsf.cpython-310-x86_64-linux-gnu.so"
evidence_root="/project/tma1/ndnsf-di/evidence/spec167/native-build-login"
mkdir -p "$evidence_root"
test -f "$base_sif"
test -f "$source_cpp"
printf '%s  %s\n' "$SPEC167_NATIVE_SOURCE_SHA256" "$source_cpp" |
  sha256sum -c - > "$evidence_root/source-cpp.sha256"
test ! -e "$output_root"
mkdir -p "$output_root"

scratch=$(mktemp -d /tmp/spec167-native-file-producer-login.XXXXXX)
cleanup() { rm -rf -- "$scratch"; }
trap cleanup EXIT INT TERM
mkdir -p "$scratch/include/ndn-base" "$scratch/include/ndnsf-app" \
  "$scratch/include/python3.10" "$scratch/include/boost" "$scratch/lib/ndn-base" \
  "$scratch/lib/ndnsf-app" "$scratch/lib/boost" "$scratch/lib/system" \
  "$scratch/build" "$scratch/test/ndnsf" "$scratch/test-libs"

{
  echo "host=$(hostname)"
  echo "baseSif=$base_sif"
  echo "sourceRoot=$source_root"
  echo "sourceSha256=$SPEC167_NATIVE_SOURCE_SHA256"
  echo "g++=$(/usr/bin/g++ --version | head -1)"
  echo "apptainer=$(apptainer version)"
} > "$evidence_root/environment.txt"

apptainer exec --cleanenv --no-mount all "$base_sif" /usr/bin/tar -cf - \
  -C /opt/ndn-base/include . | tar -xf - -C "$scratch/include/ndn-base"
apptainer exec --cleanenv --no-mount all "$base_sif" /usr/bin/tar -cf - \
  -C /opt/ndnsf-app/include . | tar -xf - -C "$scratch/include/ndnsf-app"
apptainer exec --cleanenv --no-mount all "$base_sif" /usr/bin/tar -cf - \
  -C /usr/local/include/python3.10 . | tar -xf - -C "$scratch/include/python3.10"
mapfile -t boost_names < <(
  apptainer exec --cleanenv --no-mount all "$base_sif" /usr/bin/find \
    /usr/lib/x86_64-linux-gnu -maxdepth 1 -type f -name 'libboost_*.so*' \
    -printf '%f\n'
)
test "${#boost_names[@]}" -gt 0
apptainer exec --cleanenv --no-mount all "$base_sif" /usr/bin/tar -cf - \
  -C /usr/lib/x86_64-linux-gnu "${boost_names[@]}" |
  tar -xf - -C "$scratch/lib/boost"
cp -a "$source_root/boost/." "$scratch/include/boost/"
apptainer exec --cleanenv --no-mount all "$base_sif" /usr/bin/tar -cf - \
  -C /opt/ndn-base/lib . | tar -xf - -C "$scratch/lib/ndn-base"
apptainer exec --cleanenv --no-mount all "$base_sif" /usr/bin/tar -cf - \
  -C /opt/ndnsf-app/lib . | tar -xf - -C "$scratch/lib/ndnsf-app"
apptainer exec --cleanenv --no-mount all "$base_sif" /usr/bin/tar -cf - \
  -C /usr/lib/x86_64-linux-gnu libcrypto.so.1.1 libssl.so.1.1 |
  tar -xf - -C "$scratch/lib/system"

/usr/bin/g++ -std=c++17 -O2 -fPIC -c "$source_cpp" \
  -I"$source_root" -I"$source_root/ndn-service-framework" \
  -I"$scratch/include/ndn-base" \
  -I"$scratch/include/ndnsf-app" -I"$scratch/include/python3.10" \
  -I"$source_root/pybind11/include" -I"$scratch/include/boost" \
  -o "$scratch/build/_ndnsf.o"
/usr/bin/g++ -shared "$scratch/build/_ndnsf.o" \
  -L"$scratch/lib/ndnsf-app" -L"$scratch/lib/ndn-base" \
  -L"$scratch/lib/boost" -L"$scratch/lib/system" -L/usr/lib/x86_64-linux-gnu \
  -Wl,--disable-new-dtags -Wl,-rpath,/opt/ndnsf-app/lib \
  -Wl,-rpath,/opt/ndn-base/lib \
  -lndn-service-framework -lndn-svs -lnac-abe -lndn-cxx \
  -l:libboost_chrono.so.1.71.0 -l:libboost_date_time.so.1.71.0 \
  -l:libboost_log.so.1.71.0 -l:libboost_thread.so.1.71.0 \
  -l:libboost_stacktrace_backtrace.so.1.71.0 -l:libcrypto.so.1.1 \
  -l:libssl.so.1.1 -lsqlite3 -lrt \
  -lpthread -lndnsd -o "$output_so"

cp -a "$output_so" "$scratch/test/ndnsf/"
touch "$scratch/test/ndnsf/__init__.py"
cp -L /usr/lib64/libstdc++.so.6.0.29 "$scratch/test-libs/libstdc++.so.6"
cp -L /usr/lib64/libgcc_s.so.1 "$scratch/test-libs/libgcc_s.so.1"
cp -L /usr/lib64/libstdc++.so.6.0.29 "$output_root/libstdc++.so.6.0.29"
cp -L /usr/lib64/libgcc_s.so.1 "$output_root/libgcc_s.so.1"
apptainer exec --cleanenv --no-mount all \
  --env LD_LIBRARY_PATH=/native-libs:/opt/ndnsf-app/lib:/opt/ndn-base/lib \
  --bind "$scratch/test:/native-test:ro" --bind "$scratch/test-libs:/native-libs:ro" \
  "$base_sif" /opt/venv/bin/python -c \
  'import sys; sys.path.insert(0, "/native-test"); import ndnsf._ndnsf as m; print(m.FileSegmentedObjectProducer.__name__)' \
  > "$evidence_root/import-probe.txt"

sha256sum "$output_so" > "$output_root/extension.sha256"
cat > "$output_root/manifest.json" <<EOF
{
  "schemaVersion": "spec167-native-file-producer-v1",
  "baseSif": "$base_sif",
  "sourceCppSha256": "sha256:$SPEC167_NATIVE_SOURCE_SHA256",
  "extension": "$output_so",
  "extensionSha256": "sha256:$(cut -d' ' -f1 "$output_root/extension.sha256")",
  "pythonAbi": "cpython-310-x86_64-linux-gnu",
  "persistentFileStream": true,
  "runtimeLibraries": ["libstdc++.so.6.0.29", "libgcc_s.so.1"]
}
EOF
chmod 0444 "$output_so" "$output_root/libstdc++.so.6.0.29" "$output_root/libgcc_s.so.1" \
  "$output_root/extension.sha256" "$output_root/manifest.json"
printf 'NATIVE_FILE_PRODUCER_BUILD_OK output=%s\n' "$output_so" | tee "$evidence_root/result.txt"
