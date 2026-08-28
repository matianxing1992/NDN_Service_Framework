#!/usr/bin/env bash
set -Eeuo pipefail

if test "$#" -ne 1; then
  echo "usage: $0 OUTPUT_DIR" >&2
  exit 2
fi

output_dir=$1
case "$output_dir" in
  /workspace/results/spec168-native-closure/*) ;;
  *) echo "SPEC168_NATIVE_OUTPUT_OUT_OF_SCOPE:$output_dir" >&2; exit 2 ;;
esac
test ! -e "$output_dir" || {
  echo "SPEC168_NATIVE_OUTPUT_ALREADY_EXISTS:$output_dir" >&2
  exit 2
}

work_dir=$(mktemp -d /tmp/spec168-native-closure.XXXXXX)
trap 'rm -rf "$work_dir"' EXIT
mkdir -p "$output_dir"

export CFLAGS="-O1 -g0"
export CXXFLAGS="-O1 -g0"
export HOME=/tmp

./waf configure --out="$work_dir/core" --prefix=/opt/ndnsf-app --with-examples
./waf build --out="$work_dir/core" --targets=ndn-service-framework -j1

export NDNSF_LIBRARY_DIR="$work_dir/core"
export LD_LIBRARY_PATH="$work_dir/core:/opt/ndnsf-app/lib:/opt/ndn-base/lib"

(
  cd pythonWrapper
  /opt/venv/bin/python setup.py build_ext \
    --build-temp "$work_dir/ndnsf-temp" \
    --build-lib "$work_dir/ndnsf-lib"
)
(
  cd NDNSF-DistributedRepo/pythonWrapper
  /opt/venv/bin/python setup.py build_ext \
    --build-temp "$work_dir/repo-temp" \
    --build-lib "$work_dir/repo-lib"
)

install -m 0755 "$work_dir/core/libndn-service-framework.so" \
  "$output_dir/libndn-service-framework.so.0.1.0"
install -m 0755 "$work_dir"/ndnsf-lib/ndnsf/_ndnsf.cpython-310-*.so \
  "$output_dir/"
install -m 0755 \
  "$work_dir"/repo-lib/py_repoclient/_py_repoclient.cpython-310-*.so \
  "$output_dir/"

(cd "$output_dir" && sha256sum \
  libndn-service-framework.so.0.1.0 \
  _ndnsf.cpython-310-*.so \
  _py_repoclient.cpython-310-*.so > native-closure.sha256)
printf 'SPEC168_NATIVE_CLOSURE_PASS output=%s\n' "$output_dir"
