# syntax=docker/dockerfile:1.7

ARG PYTHON_BASE_IMAGE
ARG GPU_BUILD_BASE_IMAGE
ARG GPU_RUNTIME_BASE_IMAGE

FROM ${PYTHON_BASE_IMAGE} AS python-runtime
RUN rm -f \
      /usr/local/lib/python3.10/lib-dynload/nis.*.so \
      /usr/local/lib/python3.10/lib-dynload/_tkinter.*.so

FROM ${GPU_BUILD_BASE_IMAGE} AS ml-devel
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
ARG PLATFORM_LOCK_DIGEST
ARG ML_LOCK_DIGEST
ARG DEBIAN_FRONTEND=noninteractive
ENV PATH=/opt/venv/bin:${PATH} \
    PKG_CONFIG_PATH=/opt/onnxruntime/lib/pkgconfig \
    LD_LIBRARY_PATH=/opt/onnxruntime/lib

COPY --from=python-runtime /usr/local /usr/local
COPY packaging/ndnsf-di-container/oci/layered/locks/platform.lock.json /build-contract/platform.lock.json
COPY packaging/ndnsf-di-container/oci/layered/locks/ml-runtime.lock.json /build-contract/ml-runtime.lock.json
COPY packaging/ndnsf-di-container/oci/scripts/verify-python-environment.py /build-contract/verify-python-environment.py

RUN python3 - "$PLATFORM_LOCK_DIGEST" "$ML_LOCK_DIGEST" <<'PY'
import hashlib,pathlib,sys
for path,wanted in (
  ('/build-contract/platform.lock.json',sys.argv[1]),
  ('/build-contract/ml-runtime.lock.json',sys.argv[2]),
):
 measured='sha256:'+hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()
 if measured != wanted:
  raise SystemExit('ML_LOCK_DIGEST_MISMATCH:'+path)
PY

RUN python3 - <<'PY' >/tmp/ml-runtime-packages
import json
print('\n'.join(json.load(open('/build-contract/ml-runtime.lock.json'))['pythonRuntimePackages']))
PY
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    sed -i 's|http://archive.ubuntu.com|https://archive.ubuntu.com|g; s|http://security.ubuntu.com|https://security.ubuntu.com|g' /etc/apt/sources.list && \
    rm -rf /var/lib/apt/lists/* /var/cache/apt/archives/*.deb /var/cache/apt/archives/partial/* && \
    apt-get -o Acquire::Retries=3 update && \
    apt-get install -y --no-install-recommends \
      ca-certificates curl $(cat /tmp/ml-runtime-packages)

RUN python3 - <<'PY' >/tmp/ort.env
import json,shlex
v=json.load(open('/build-contract/ml-runtime.lock.json'))['onnxRuntimeCpp']
for key in ('url','sha256','bytes','version'):
 print('ORT_'+key.upper()+'='+shlex.quote(str(v[key])))
PY
RUN --mount=type=cache,id=spec158-ort-download,target=/var/cache/spec158-downloads,sharing=locked \
    set -eu; . /tmp/ort.env; \
    archive=/var/cache/spec158-downloads/onnxruntime.tgz; \
    if [ -f "$archive" ] && [ "$(stat -c %s "$archive")" = "$ORT_BYTES" ]; then \
      echo "$ORT_SHA256  $archive" | sha256sum -c - || rm -f "$archive"; \
    fi; \
    if [ ! -f "$archive" ] || [ "$(stat -c %s "$archive")" != "$ORT_BYTES" ]; then \
      attempt=1; \
      while ! curl -fL -C - -o "$archive" "$ORT_URL"; do \
        test "$attempt" -lt 10 || exit 1; \
        attempt=$((attempt + 1)); \
        sleep "$attempt"; \
      done; \
    fi; \
    echo "$ORT_SHA256  $archive" | sha256sum -c -; \
    test "$(stat -c %s "$archive")" = "$ORT_BYTES"; \
    mkdir -p /opt/onnxruntime; \
    tar -xzf "$archive" --strip-components=1 -C /opt/onnxruntime; \
    test -f /opt/onnxruntime/include/onnxruntime_cxx_api.h; \
    test -f /opt/onnxruntime/lib/libonnxruntime.so; \
    test -f /opt/onnxruntime/lib/libonnxruntime_providers_cuda.so
RUN printf '%s\n' \
      'prefix=/opt/onnxruntime' \
      'exec_prefix=${prefix}' \
      'libdir=${prefix}/lib' \
      'includedir=${prefix}/include' \
      '' \
      'Name: onnxruntime' \
      'Description: ONNX Runtime GPU C++ API' \
      'Version: 1.20.0' \
      'Libs: -L${libdir} -lonnxruntime' \
      'Cflags: -I${includedir}' \
      >/opt/onnxruntime/lib/pkgconfig/onnxruntime.pc

RUN python3.10 -m venv /opt/venv && \
    /opt/venv/bin/pip install --no-cache-dir --upgrade \
      pip==25.0.1 setuptools==75.8.0 wheel==0.45.1
RUN python3 - <<'PY' >/tmp/python-requirements
import json
lock=json.load(open('/build-contract/ml-runtime.lock.json'))
print('\n'.join(
 f'{name}=={version}' for name,version in sorted(lock['pythonPackages'].items())
 if name != 'torch'
))
PY
RUN --mount=type=cache,target=/root/.cache/pip,sharing=locked \
    /opt/venv/bin/pip install --no-deps --requirement /tmp/python-requirements && \
    /opt/venv/bin/pip install --no-deps \
      --index-url https://download.pytorch.org/whl/cu124 torch==2.6.0
RUN python3 -c \
      'import json; assert json.load(open("/build-contract/ml-runtime.lock.json"))["onnxRuntimeExcludedOptionalProviders"] == ["TensorrtExecutionProvider"]' && \
    test -f /opt/venv/lib/python3.10/site-packages/onnxruntime/capi/libonnxruntime_providers_cuda.so && \
    rm -f /opt/onnxruntime/lib/libonnxruntime_providers_tensorrt.so \
      /opt/venv/lib/python3.10/site-packages/onnxruntime/capi/libonnxruntime_providers_tensorrt.so && \
    test ! -e /opt/onnxruntime/lib/libonnxruntime_providers_tensorrt.so
RUN /opt/venv/bin/python /build-contract/verify-python-environment.py \
      --lock /build-contract/ml-runtime.lock.json && \
    /opt/venv/bin/python -c \
      'import onnxruntime,torch,transformers; print(torch.__version__,onnxruntime.__version__,transformers.__version__)'
RUN python3.10 -m venv /opt/runtime-venv && \
    /opt/runtime-venv/bin/pip install --no-cache-dir --upgrade \
      pip==25.0.1 setuptools==75.8.0 wheel==0.45.1
RUN python3 - <<'PY' >/tmp/deployment-python-requirements
import json
lock=json.load(open('/build-contract/ml-runtime.lock.json'))
for name, version in sorted(lock['deploymentPythonPackages'].items()):
    print(f'{name}=={version}')
PY
RUN --mount=type=cache,target=/root/.cache/pip,sharing=locked \
    /opt/runtime-venv/bin/pip install --no-deps --requirement /tmp/deployment-python-requirements && \
    /opt/runtime-venv/bin/python /build-contract/verify-python-environment.py \
      --lock /build-contract/ml-runtime.lock.json --package-set deploymentPythonPackages && \
    ! /opt/runtime-venv/bin/python -c 'import torch' && \
    ! /opt/runtime-venv/bin/python -c 'import transformers'
RUN install -d /opt/ndnsf-di-ml/manifest && \
    install -m 0644 /build-contract/platform.lock.json \
      /build-contract/ml-runtime.lock.json /opt/ndnsf-di-ml/manifest/ && \
    printf '%s\n' "$PLATFORM_LOCK_DIGEST" >/opt/ndnsf-di-ml/manifest/platform-lock-digest && \
    printf '%s\n' "$ML_LOCK_DIGEST" >/opt/ndnsf-di-ml/manifest/ml-lock-digest
LABEL org.ndnsf.di.layer="ml-devel" \
      org.ndnsf.di.platform-lock="${PLATFORM_LOCK_DIGEST}" \
      org.ndnsf.di.ml-lock="${ML_LOCK_DIGEST}" \
      org.ndnsf.di.models-included="false"

FROM ${GPU_RUNTIME_BASE_IMAGE} AS ml-runtime
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
ARG PLATFORM_LOCK_DIGEST
ARG ML_LOCK_DIGEST
ARG DEBIAN_FRONTEND=noninteractive
COPY --from=python-runtime /usr/local /usr/local
COPY --from=ml-devel /opt/runtime-venv /opt/venv
COPY --from=ml-devel /opt/onnxruntime /opt/onnxruntime
COPY --from=ml-devel /opt/ndnsf-di-ml /opt/ndnsf-di-ml
COPY packaging/ndnsf-di-container/oci/scripts/verify-python-environment.py /usr/local/bin/verify-python-environment.py
COPY packaging/ndnsf-di-container/oci/scripts/verify-runtime-closure.py /usr/local/bin/verify-runtime-closure.py
RUN python3 - <<'PY' >/tmp/ml-runtime-packages
import json
print('\n'.join(json.load(open('/opt/ndnsf-di-ml/manifest/ml-runtime.lock.json'))['pythonRuntimePackages']))
PY
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    sed -i 's|http://archive.ubuntu.com|https://archive.ubuntu.com|g; s|http://security.ubuntu.com|https://security.ubuntu.com|g' /etc/apt/sources.list && \
    rm -rf /var/lib/apt/lists/* /var/cache/apt/archives/*.deb /var/cache/apt/archives/partial/* && \
    apt-get -o Acquire::Retries=3 update && \
    apt-get install -y --no-install-recommends \
      ca-certificates $(cat /tmp/ml-runtime-packages) && \
    rm -rf /var/lib/apt/lists/* /tmp/ml-runtime-packages && \
    ldconfig
ENV PATH=/opt/venv/bin:${PATH} \
    PKG_CONFIG_PATH=/opt/onnxruntime/lib/pkgconfig \
    LD_LIBRARY_PATH=/opt/onnxruntime/lib \
    PYTHONNOUSERSITE=1 \
    PYTHONDONTWRITEBYTECODE=1
RUN /opt/venv/bin/python /usr/local/bin/verify-python-environment.py \
      --lock /opt/ndnsf-di-ml/manifest/ml-runtime.lock.json \
      --package-set deploymentPythonPackages && \
    /opt/venv/bin/python -c 'import onnxruntime,tokenizers' && \
    ! /opt/venv/bin/python -c 'import torch' && \
    ! /opt/venv/bin/python -c 'import transformers' && \
    python3 /usr/local/bin/verify-runtime-closure.py \
      --root /opt/venv --root /opt/onnxruntime \
      --root /usr/local/bin --root /usr/local/lib/python3.10 && \
    rm -rf /root/.cache/huggingface && \
    test ! -e /root/.cache/huggingface
LABEL org.ndnsf.di.layer="ml-runtime" \
      org.ndnsf.di.platform-lock="${PLATFORM_LOCK_DIGEST}" \
      org.ndnsf.di.ml-lock="${ML_LOCK_DIGEST}" \
      org.ndnsf.di.models-included="false"
