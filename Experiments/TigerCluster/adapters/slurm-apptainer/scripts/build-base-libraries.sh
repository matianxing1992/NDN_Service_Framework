#!/bin/bash
# Container-only body of library-runtime.def.in. No host or Tiger builds.
set -euo pipefail
test -f /build-input/source/source-seal.json
export NDNSF_CONTAINER_BUILD=1
unset PYTHONPATH PYTHONHOME PYTHONOPTIMIZE
export PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/venv/bin
export CC=/usr/bin/gcc CXX=/usr/bin/g++
export CFLAGS='-O1 -g0 -B/usr/bin/'
export CXXFLAGS='-O1 -g0 -B/usr/bin/ -DBOOST_PHOENIX_DONT_USE_PREPROCESSED_FILES'
export LDFLAGS='-B/usr/bin/ -Wl,-rpath,/opt/ndnsf-di/current/lib'
export LD_LIBRARY_PATH=/opt/ndnsf-di/current/lib:/opt/onnxruntime/lib
export PKG_CONFIG_PATH=/opt/ndnsf-di/current/lib/pkgconfig:/opt/onnxruntime/lib/pkgconfig
export CPLUS_INCLUDE_PATH=/opt/ndnsf-di/current/include
export LIBRARY_PATH=/opt/ndnsf-di/current/lib
export PYTHONNOUSERSITE=1 DEBIAN_FRONTEND=noninteractive

/opt/venv/bin/python - <<'PY'
import hashlib, json
from pathlib import Path
root = Path('/build-input/source')
seal = json.loads((root / 'source-seal.json').read_text())
assert seal['sourceSelection'] == 'base-libraries-v1'
for filename, record in [('workspace.tar', seal['archive']),
                        *[(v['archive']['path'].split('/')[-1], v['archive'])
                          for v in seal['dependencies'].values()]]:
    data = (root / filename).read_bytes()
    assert len(data) == record['bytes']
    assert 'sha256:' + hashlib.sha256(data).hexdigest() == record['sha256']
PY

# Remove inherited applications before compiling any new library consumers.
rm -rf /src/ndnsf /src/ndn-svs /src/nac-abe /src/ndn-sd /opt/ndnsf-di/replay
rm -rf /opt/ndnsf-stage /opt/ndnsf-candidate
rm -rf /opt/ndnsf-di/current/manifest
rm -f /opt/ndnsf-di/current/bin/{di-native-provider,di-native-fault-provider,App_ServiceController}
rm -rf /opt/venv/lib/python3.10/site-packages/{ndnsf,py_repoclient,ndnsf_distributed_inference}
rm -rf /opt/venv/lib/python3.10/site-packages/{ndnsf,py_repoclient,ndnsf_di,ndnsf_distributed_inference}-*.dist-info
sed -i 's|http://archive.ubuntu.com|https://archive.ubuntu.com|g; s|http://security.ubuntu.com|https://security.ubuntu.com|g' /etc/apt/sources.list
apt-get -o Acquire::Retries=3 update
apt-get install -y --no-install-recommends build-essential cmake pkg-config libgmp-dev libssl-dev libsqlite3-dev libboost-all-dev libpcap-dev protobuf-compiler libprotobuf-dev libgtkmm-3.0-dev ca-certificates python3.10-dev
rm -rf /var/lib/apt/lists/* /var/cache/apt/archives/*.deb
mkdir -p /src/{ndnsf,ndn-svs,nac-abe,ndn-sd}
tar -xf /build-input/source/workspace.tar -C /src/ndnsf
tar -xf /build-input/source/nacAbe.tar -C /src/nac-abe
tar -xf /build-input/source/ndn-svs.tar -C /src/ndn-svs
tar -xf /build-input/source/ndnSd.tar -C /src/ndn-sd
cmake -S /src/nac-abe -B /src/nac-abe/build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE='-O1 -DNDEBUG -g0' -DHAVE_TESTS=OFF -DBUILD_EXAMPLES=OFF \
    -DCMAKE_INSTALL_PREFIX=/opt/ndnsf-di/current -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_PREFIX_PATH=/opt/ndnsf-di/current
cmake --build /src/nac-abe/build --parallel 2
cmake --install /src/nac-abe/build
for project in ndn-svs ndn-sd; do
    cd /src/"$project"
    ./waf configure --prefix=/opt/ndnsf-di/current --libdir=/opt/ndnsf-di/current/lib \
        --boost-includes=/usr/include --boost-libs=/usr/lib/x86_64-linux-gnu
    ./waf -j2
    ./waf install -j2
done
cd /src/ndnsf
./waf configure --runtime-libraries-only --disable-local-dependency-prefix \
    --prefix=/opt/ndnsf-di/current --libdir=/opt/ndnsf-di/current/lib \
    --nac-abe-prefix=/opt/ndnsf-di/current \
    --boost-includes=/usr/include --boost-libs=/usr/lib/x86_64-linux-gnu
./waf -j2
./waf install -j2
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
cd /
rm -rf /src/ndnsf /src/ndn-svs /src/nac-abe /src/ndn-sd /build-input /root/.cache/pip
