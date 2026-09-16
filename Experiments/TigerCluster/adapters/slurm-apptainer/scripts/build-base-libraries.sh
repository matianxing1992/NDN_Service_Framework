#!/bin/bash
# Container-only base refresh, adapted from TigerClusterExperiments c3b312d0.
# Core/DI/SVS/NDNSD/NAC-ABE and their bindings belong to the APP.
set -euo pipefail
test -d /.singularity.d || { echo BASE_CONTAINER_REQUIRED >&2; exit 2; }
test -f /build-input/base-runtime.lock.json
unset PYTHONPATH PYTHONHOME PYTHONOPTIMIZE
export PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/venv/bin
export PYTHONNOUSERSITE=1
export LD_LIBRARY_PATH=/opt/ndn-base/lib:/opt/onnxruntime/lib
/opt/venv/bin/python /build-input/base-runtime.py prepare \
    --lock /build-input/base-runtime.lock.json --wheels /build-input/wheels
/opt/venv/bin/python /opt/ndn-base/manifest/base-runtime.py verify
