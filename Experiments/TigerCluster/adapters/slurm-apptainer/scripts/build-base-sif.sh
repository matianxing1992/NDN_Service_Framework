#!/usr/bin/env bash
set -euo pipefail
usage() {
  cat >&2 <<'EOF'
usage: build-base-sif.sh --parent-sif PATH --parent-sha256 HEX \
  --numpy-wheel PATH --numpy-wheel-sha256 SHA256:HEX \
  --output PATH --record PATH --apptainer PATH --expected-apptainer VERSION
EOF
  exit 2
}
parent_sif=''; parent_sha=''; numpy_wheel=''; numpy_sha=''
output=''; record=''; apptainer_bin=''; expected_version=''
while (($#)); do
  case "$1" in
    --parent-sif) parent_sif=$2; shift 2 ;;
    --parent-sha256) parent_sha=$2; shift 2 ;;
    --numpy-wheel) numpy_wheel=$2; shift 2 ;;
    --numpy-wheel-sha256) numpy_sha=$2; shift 2 ;;
    --output) output=$2; shift 2 ;;
    --record) record=$2; shift 2 ;;
    --apptainer) apptainer_bin=$2; shift 2 ;;
    --expected-apptainer) expected_version=$2; shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$parent_sif" ] && [ -n "$parent_sha" ] && [ -n "$numpy_wheel" ] &&
  [ -n "$numpy_sha" ] && [ -n "$output" ] && [ -n "$record" ] &&
  [ -n "$apptainer_bin" ] && [ -n "$expected_version" ] || usage
parent_sif=$(readlink -f "$parent_sif")
numpy_wheel=$(readlink -f "$numpy_wheel")
output=$(readlink -m "$output")
record=$(readlink -m "$record")
apptainer_bin=$(readlink -f "$apptainer_bin")
[ -f "$parent_sif" ] || { echo BASE_SIF_PARENT_MISSING >&2; exit 4; }
[ -f "$numpy_wheel" ] || { echo BASE_SIF_NUMPY_WHEEL_MISSING >&2; exit 4; }
[ -x "$apptainer_bin" ] || { echo BASE_SIF_APPTAINER_NOT_EXECUTABLE >&2; exit 4; }
[ ! -e "$output" ] || { echo BASE_SIF_OUTPUT_EXISTS >&2; exit 4; }
[ ! -e "$record" ] || { echo BASE_SIF_RECORD_EXISTS >&2; exit 4; }
config=$(printenv SPEC186_APPTAINER_CONFIG 2>/dev/null || true)
root_mapped=$(printenv SPEC186_APPTAINER_ROOT_MAPPED 2>/dev/null || printf '0')
mksquashfs_args=$(printenv SPEC186_MKSQUASHFS_ARGS 2>/dev/null || printf '%s' '-processors 1 -no-xattrs')
normalize_version() {
  printf '%s\n' "$1" | sed -E 's/[^0-9]*([0-9]+\.[0-9]+\.[0-9]+).*/\1/'
}
apptainer_run() {
  if [ -n "$config" ]; then
    "$apptainer_bin" -c "$config" "$@"
  else
    "$apptainer_bin" "$@"
  fi
}
local_version=$(apptainer_run --version)
[ "$(normalize_version "$local_version")" = "$(normalize_version "$expected_version")" ] || {
  echo "BASE_SIF_APPTAINER_VERSION_MISMATCH local=$local_version expected=$expected_version" >&2
  exit 4
}
parent_actual=$(sha256sum "$parent_sif" | awk '{print $1}')
parent_expected=$(printf '%s' "$parent_sha" | sed 's/^sha256://')
[ "$parent_actual" = "$parent_expected" ] || {
  echo "BASE_SIF_PARENT_SHA256_MISMATCH expected=$parent_expected actual=$parent_actual" >&2
  exit 4
}
numpy_actual=$(sha256sum "$numpy_wheel" | awk '{print $1}')
numpy_expected=$(printf '%s' "$numpy_sha" | sed 's/^sha256://')
[ "$numpy_actual" = "$numpy_expected" ] || {
  echo "BASE_SIF_NUMPY_WHEEL_SHA256_MISMATCH expected=$numpy_expected actual=$numpy_actual" >&2
  exit 4
}
apptainer_run sif list "$parent_sif" >/dev/null
private_json=$(python3 - "$numpy_wheel" <<'PY'
import json, sys
from pathlib import Path
from zipfile import ZipFile
with ZipFile(Path(sys.argv[1])) as archive:
    members = sorted(Path(n).name for n in archive.namelist()
                     if n.startswith("numpy.libs/") and not n.endswith("/"))
expected = sorted(["libgfortran-040039e1.so.5.0.0",
                   "libopenblas64_p-r0-0cf96a72.3.23.dev.so",
                   "libquadmath-96973f99.so.0.0.0"])
if members != expected:
    raise SystemExit("BASE_SIF_NUMPY_PRIVATE_LIB_SET_MISMATCH " + json.dumps(members))
print(json.dumps(expected, separators=(",", ":")))
PY
)
definition=$(printf '%s' "$output" | sed 's/\.sif$/.def/')
partial=$output.partial
[ ! -e "$definition" ] || { echo BASE_SIF_DEFINITION_EXISTS >&2; exit 4; }
[ ! -e "$partial" ] || { echo BASE_SIF_PARTIAL_OUTPUT_EXISTS >&2; exit 4; }
[ ! -e "$record.partial" ] || { echo BASE_SIF_PARTIAL_RECORD_EXISTS >&2; exit 4; }
mkdir -p "$(dirname "$output")" "$(dirname "$record")"
cat > "$definition" <<EOF
Bootstrap: localimage
From: $parent_sif

%labels
    org.ndnsf.di.base-repair numpy-private-libs-and-setuptools-v1
    org.ndnsf.di.parent-sif-sha256 sha256:$parent_actual
    org.ndnsf.di.numpy-wheel-sha256 sha256:$numpy_actual
    org.ndnsf.di.apptainer-version $(normalize_version "$local_version")

%files
    $numpy_wheel /build-input/numpy.whl

%post
    set -eu
    test -d /dev || mkdir /dev
    test -d /tmp || mkdir /tmp
    test -d /var/tmp || mkdir /var/tmp
    install -d /opt/venv/lib/python3.10/site-packages/numpy.libs
    /usr/bin/python3 - /build-input/numpy.whl <<'PY'
from pathlib import Path
from zipfile import ZipFile
wheel = Path('/build-input/numpy.whl')
destination = Path('/opt/venv/lib/python3.10/site-packages/numpy.libs')
expected = {'libgfortran-040039e1.so.5.0.0',
            'libopenblas64_p-r0-0cf96a72.3.23.dev.so',
            'libquadmath-96973f99.so.0.0.0'}
for child in destination.iterdir():
    if child.is_file() or child.is_symlink():
        child.unlink()
with ZipFile(wheel) as archive:
    members = {Path(n).name: n for n in archive.namelist()
               if n.startswith('numpy.libs/') and not n.endswith('/')}
    assert set(members) == expected, (set(members), expected)
    for name in sorted(expected):
        target = destination / name
        target.write_bytes(archive.read(members[name]))
        target.chmod(0o755)
assert {p.name for p in destination.iterdir() if p.is_file()} == expected
resource = Path('/opt/venv/lib/python3.10/site-packages/setuptools/_vendor/jaraco/text/Lorem ipsum.txt')
resource.parent.mkdir(parents=True, exist_ok=True)
resource.touch()
PY
    rm -f /build-input/numpy.whl
%test
    /opt/venv/bin/python - <<'PY'
import subprocess
from pathlib import Path
import numpy
assert numpy.__version__ == '1.26.4', numpy.__version__
libs = Path(numpy.__file__).parent.parent / 'numpy.libs'
expected = {'libgfortran-040039e1.so.5.0.0',
            'libopenblas64_p-r0-0cf96a72.3.23.dev.so',
            'libquadmath-96973f99.so.0.0.0'}
assert {p.name for p in libs.iterdir() if p.is_file()} == expected
from numpy.core import _multiarray_umath
ldd = subprocess.run(['ldd', _multiarray_umath.__file__],
                     text=True, capture_output=True, check=True)
assert 'not found' not in ldd.stdout, ldd.stdout
print('BASE_NUMPY_IMPORT_PASS')
PY
EOF
build_args=()
if [ "$root_mapped" = 1 ]; then
  build_args+=(--ignore-subuid --ignore-fakeroot-command)
fi
build_args+=(--mksquashfs-args "$mksquashfs_args")
echo "BASE_SIF_BUILD_START parent=sha256:$parent_actual wheel=sha256:$numpy_actual output=$output" >&2
apptainer_run build "${build_args[@]}" "$partial" "$definition"
mv "$partial" "$output"
output_actual=$(sha256sum "$output" | awk '{print $1}')
output_bytes=$(stat -c '%s' "$output")
apptainer_binary_sha=$(sha256sum "$apptainer_bin" | awk '{print $1}')
probe_script=$(cat <<'PY'
import subprocess
from pathlib import Path
import numpy
assert numpy.__version__ == '1.26.4', numpy.__version__
libs = Path(numpy.__file__).parent.parent / 'numpy.libs'
expected = {'libgfortran-040039e1.so.5.0.0',
            'libopenblas64_p-r0-0cf96a72.3.23.dev.so',
            'libquadmath-96973f99.so.0.0.0'}
assert {p.name for p in libs.iterdir() if p.is_file()} == expected
from numpy.core import _multiarray_umath
ldd = subprocess.run(['ldd', _multiarray_umath.__file__],
                     text=True, capture_output=True, check=True)
assert 'not found' not in ldd.stdout, ldd.stdout
print('BASE_NUMPY_IMPORT_PASS')
PY
)
probe_output=$(apptainer_run exec --cleanenv --containall --no-home --pwd / \
  --no-mount dev "$output" /opt/venv/bin/python - <<<"$probe_script")
APPTAINER_BIN="$apptainer_bin" python3 - "$record" "$definition" "$output" \
  "$parent_sif" "$numpy_wheel" "$parent_actual" "$numpy_actual" \
  "$output_actual" "$output_bytes" "$apptainer_binary_sha" \
  "$local_version" "$private_json" "$probe_output" <<'PY'
import hashlib, json, os, sys
from pathlib import Path
args = sys.argv[1:]
record, definition, output, parent, wheel = args[:5]
parent_sha, wheel_sha, output_sha, output_bytes = args[5:9]
apptainer_sha, version, private_json, probe_output = args[9:13]
body = {
  'schemaVersion': 'spec186-base-sif-build-v1',
  'status': 'PASS',
  'repair': 'numpy-private-libs-and-setuptools-v1',
  'parent': {'path': str(Path(parent).resolve()), 'sha256': 'sha256:' + parent_sha,
             'bytes': Path(parent).stat().st_size},
  'numpyWheel': {'path': str(Path(wheel).resolve()), 'sha256': 'sha256:' + wheel_sha,
                 'bytes': Path(wheel).stat().st_size,
                 'privateLibraries': json.loads(private_json)},
  'definition': {'path': str(Path(definition).resolve()),
                 'sha256': 'sha256:' + hashlib.sha256(Path(definition).read_bytes()).hexdigest()},
  'output': {'path': str(Path(output).resolve()), 'sha256': 'sha256:' + output_sha,
             'bytes': int(output_bytes)},
  'apptainer': {'path': os.environ['APPTAINER_BIN'], 'version': version,
                'binarySha256': 'sha256:' + apptainer_sha},
  'validation': {'runtimeProbe': probe_output.splitlines()[-1],
                 'sifList': 'PASS', 'runtimeMountMode': 'no-mount-dev'},
}
Path(record + '.partial').write_text(
    json.dumps(body, indent=2, sort_keys=True) + '\n', encoding='utf-8')
Path(record + '.partial').replace(record)
PY
rm -f "$definition"
echo "BASE_SIF_BUILD_PASS output=$output sha256:$output_actual record=$record" >&2
