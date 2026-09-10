#!/bin/bash
# Container-only body of library-runtime.def.in. No host or Tiger builds.
set -euo pipefail
test -f /build-input/source/source-seal.json
export NDNSF_CONTAINER_BUILD=1
unset PYTHONPATH PYTHONHOME PYTHONOPTIMIZE
export PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/venv/bin
export CC=/usr/bin/gcc CXX=/usr/bin/g++
BUILD_JOBS="${NDNSF_BUILD_JOBS:-2}"
case "${BUILD_JOBS}" in
    1|2|3|4) ;;
    *) echo "BASE_BUILD_JOBS must be an integer in [1,4]" >&2; exit 2 ;;
esac
BUILD_OPT="${NDNSF_BUILD_OPT:-O1}"
case "${BUILD_OPT}" in
    O0|O1) ;;
    *) echo "BASE_BUILD_OPT must be O0 or O1" >&2; exit 2 ;;
esac
export CFLAGS="-${BUILD_OPT} -g0 -B/usr/bin/"
export CXXFLAGS="-${BUILD_OPT} -g0 -B/usr/bin/ -DBOOST_PHOENIX_DONT_USE_PREPROCESSED_FILES"
export LDFLAGS='-B/usr/bin/ -Wl,-rpath,/opt/ndnsf-di/current/lib'
export LD_LIBRARY_PATH=/opt/ndnsf-di/current/lib:/opt/onnxruntime/lib
export PKG_CONFIG_PATH=/opt/ndnsf-di/current/lib/pkgconfig:/opt/onnxruntime/lib/pkgconfig
export CPLUS_INCLUDE_PATH=/opt/ndnsf-di/current/include
export LIBRARY_PATH=/opt/ndnsf-di/current/lib
export PYTHONNOUSERSITE=1 DEBIAN_FRONTEND=noninteractive

/opt/venv/bin/python - <<'PY'
import hashlib, json, sysconfig, os, importlib.util, subprocess
from pathlib import Path
root = Path('/build-input/source')
seal = json.loads((root / 'source-seal.json').read_text())
assert seal['sourceSelection'] == 'base-libraries-v1'
include = Path(sysconfig.get_path('include'))
assert (include / 'Python.h').is_file(), 'BASE_PYTHON_HEADERS_MISSING'
assert (include / 'pyconfig.h').is_file(), 'BASE_PYTHON_CONFIG_MISSING'
for filename, record in [('workspace.tar', seal['archive']),
                        *[(v['archive']['path'].split('/')[-1], v['archive'])
                          for v in seal['dependencies'].values()]]:
    data = (root / filename).read_bytes()
    assert len(data) == record['bytes']
    assert 'sha256:' + hashlib.sha256(data).hexdigest() == record['sha256']
if os.environ.get('NDNSF_REUSE_BASE_DEPENDENCIES', '0') not in ('0', '1'):
    raise ValueError('BASE_REUSE_MODE')
if os.environ.get('NDNSF_REUSE_BASE_DEPENDENCIES') == '1':
    manifest = Path('/opt/ndnsf-di/current/manifest')
    spec = importlib.util.spec_from_file_location('renderer', '/build-input/render-library-runtime.py')
    renderer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(renderer)
    previous = json.loads((manifest / 'base-source-seal.json').read_text())
    renderer.validate_dependency_reuse(previous, seal)
    subprocess.run(['/opt/venv/bin/python', str(manifest / 'verify-base-runtime.py'), 'verify'], check=True)
    libraries = ['/opt/ndnsf-di/current/lib/' + name for name in
                 ('libnac-abe.so', 'libndn-svs.so', 'libndnsd.so', 'libndn-cxx.so')]
    receipt = dict(schema='spec183-base-dependency-reuse-v1',
                   parentSifSha256=os.environ['NDNSF_PARENT_SIF_SHA256'],
                   previousSourceSealSha256=renderer.digest(manifest / 'base-source-seal.json'),
                   sourceSealSha256=renderer.digest(root / 'source-seal.json'),
                   preservedLibraries={path: renderer.digest(Path(path)) for path in libraries})
    Path('/build-input/dependency-reuse.json').write_text(json.dumps(receipt, indent=2) + '\n')
PY

# Remove inherited applications before compiling any new library consumers.
rm -rf /src/ndnsf /src/ndn-svs /src/nac-abe /src/ndn-sd /opt/ndnsf-di/replay
rm -rf /opt/ndnsf-stage /opt/ndnsf-candidate
rm -rf /opt/ndnsf-di/current/manifest
rm -f /opt/ndnsf-di/current/bin/{di-native-provider,di-native-fault-provider,App_ServiceController}
rm -rf /opt/venv/lib/python3.10/site-packages/{ndnsf,py_repoclient,ndnsf_distributed_inference}
rm -rf /opt/venv/lib/python3.10/site-packages/{ndnsf,py_repoclient,ndnsf_di,ndnsf_distributed_inference}-*.dist-info
if [ "${NDNSF_REUSE_BASE_DEPENDENCIES:-0}" = 0 ]; then
sed -i 's|http://archive.ubuntu.com|https://archive.ubuntu.com|g; s|http://security.ubuntu.com|https://security.ubuntu.com|g' /etc/apt/sources.list
apt-get -o Acquire::Retries=3 update
apt-get install -y --no-install-recommends build-essential cmake pkg-config libgmp-dev libssl-dev libsqlite3-dev libboost-all-dev libpcap-dev protobuf-compiler libprotobuf-dev libgtkmm-3.0-dev ca-certificates
rm -rf /var/lib/apt/lists/* /var/cache/apt/archives/*.deb
fi
mkdir -p /src/{ndnsf,ndn-svs,nac-abe,ndn-sd}
tar -xf /build-input/source/workspace.tar -C /src/ndnsf
tar -xf /build-input/source/nacAbe.tar -C /src/nac-abe
tar -xf /build-input/source/ndn-svs.tar -C /src/ndn-svs
tar -xf /build-input/source/ndnSd.tar -C /src/ndn-sd
# Some handoff archives were produced from an extracted source tree whose
# executable bits had already been flattened.  Waf is part of the sealed
# source contract, so restore its required mode before invoking it rather
# than relying on the mode of an intermediate tar member.
chmod 0755 /src/ndnsf/waf /src/ndn-svs/waf /src/ndn-sd/waf
if [ "${NDNSF_REUSE_BASE_DEPENDENCIES:-0}" = 0 ]; then
cmake -S /src/nac-abe -B /src/nac-abe/build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE='-O1 -DNDEBUG -g0' -DHAVE_TESTS=OFF -DBUILD_EXAMPLES=OFF \
    -DCMAKE_INSTALL_PREFIX=/opt/ndnsf-di/current -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_PREFIX_PATH=/opt/ndnsf-di/current
cmake --build /src/nac-abe/build --parallel "${BUILD_JOBS}"
cmake --install /src/nac-abe/build
for project in ndn-svs ndn-sd; do
    cd /src/"$project"
    ./waf configure --prefix=/opt/ndnsf-di/current --libdir=/opt/ndnsf-di/current/lib \
        --boost-includes=/usr/include --boost-libs=/usr/lib/x86_64-linux-gnu
    ./waf -j"${BUILD_JOBS}"
    ./waf install -j"${BUILD_JOBS}"
done
fi
cd /src/ndnsf
./waf configure --runtime-libraries-only --disable-local-dependency-prefix \
    --prefix=/opt/ndnsf-di/current --libdir=/opt/ndnsf-di/current/lib \
    --nac-abe-prefix=/opt/ndnsf-di/current \
    --boost-includes=/usr/include --boost-libs=/usr/lib/x86_64-linux-gnu
./waf -j"${BUILD_JOBS}"
./waf install -j"${BUILD_JOBS}"
export NDNSF_NAC_ABE_PREFIX=/opt/ndnsf-di/current
export NDNSF_LIBRARY_DIR=/opt/ndnsf-di/current/lib
/opt/venv/bin/pip install --no-index --no-deps /build-input/wheels/pybind11-2.13.6-py3-none-any.whl
for package in pythonWrapper NDNSF-DistributedRepo/pythonWrapper; do
    /opt/venv/bin/pip install --no-index --no-deps --no-build-isolation /src/ndnsf/"$package"
done
mkdir -p /opt/ndnsf-di/current/manifest
cp /build-input/source/source-seal.json /opt/ndnsf-di/current/manifest/base-source-seal.json
cp /build-input/verify-base-runtime.py /opt/ndnsf-di/current/manifest/
/opt/venv/bin/python /opt/ndnsf-di/current/manifest/verify-base-runtime.py record
if [ "${NDNSF_REUSE_BASE_DEPENDENCIES:-0}" = 1 ]; then
/opt/venv/bin/python - <<'PY'
import hashlib, json
from pathlib import Path
receipt = json.loads(Path('/build-input/dependency-reuse.json').read_text())
for path, expected in receipt['preservedLibraries'].items():
    assert 'sha256:' + hashlib.sha256(Path(path).read_bytes()).hexdigest() == expected, 'BASE_REUSE_LIBRARY_CHANGED:' + path
receipt['status'] = 'PASS'
Path('/opt/ndnsf-di/current/manifest/base-dependency-reuse.json').write_text(json.dumps(receipt, indent=2) + '\n')
PY
fi
cd /
rm -rf /src/ndnsf /src/ndn-svs /src/nac-abe /src/ndn-sd /build-input /root/.cache/pip
