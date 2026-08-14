# syntax=docker/dockerfile:1.7

ARG NDN_DEVEL_IMAGE
ARG NDN_RUNTIME_IMAGE

FROM ${NDN_DEVEL_IMAGE} AS app-builder
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
ARG APP_LOCK_DIGEST
ARG APP_SEAL_DIGEST
ARG BUILD_JOBS=2
ARG DEBIAN_FRONTEND=noninteractive
ENV APP_PREFIX=/opt/ndnsf-app \
    PATH=/opt/venv/bin:/opt/ndnsf-app/bin:/opt/ndn-base/bin:${PATH} \
    PKG_CONFIG_PATH=/opt/ndnsf-app/lib/pkgconfig:/opt/ndn-base/lib/pkgconfig:/opt/onnxruntime/lib/pkgconfig \
    LD_LIBRARY_PATH=/opt/ndnsf-app/lib:/opt/ndn-base/lib:/opt/onnxruntime/lib \
    PYTHONPATH=/opt/ndnsf-app/python

COPY packaging/ndnsf-di-container/oci/layered/locks/app-runtime.lock.json /build-contract/app-runtime.lock.json
COPY packaging/ndnsf-di-container/oci/layered/patches/ndn-svs-boost-1.71.patch /build-contract/ndn-svs-boost-1.71.patch
COPY packaging/ndnsf-di-container/oci/layered/scripts/prepare-layer-seals.py /build-contract/prepare-layer-seals.py
COPY packaging/ndnsf-di-container/oci/scripts/derive-runtime-packages.py /build-contract/derive-runtime-packages.py
COPY --from=app_seal / /build-contract/seal/
RUN python3 /build-contract/prepare-layer-seals.py verify \
      --lock /build-contract/app-runtime.lock.json \
      --output /build-contract/seal | grep -Fx "$APP_SEAL_DIGEST" && \
    python3 - "$APP_LOCK_DIGEST" <<'PY'
import hashlib,pathlib,sys
measured='sha256:'+hashlib.sha256(pathlib.Path('/build-contract/app-runtime.lock.json').read_bytes()).hexdigest()
if measured != sys.argv[1]:
 raise SystemExit('APP_LOCK_DIGEST_MISMATCH')
PY
RUN set -eu; mkdir -p /src/ndn-svs /src/NDNSD /src/ndnsf; \
    tar -xf /build-contract/seal/archives/ndn-svs.tar -C /src/ndn-svs; \
    tar -xf /build-contract/seal/archives/NDNSD.tar -C /src/NDNSD; \
    tar -xf /build-contract/seal/archives/ndnsf-workspace.tar -C /src/ndnsf
RUN python3 - <<'PY'
import hashlib
import json
from pathlib import Path

lock = json.loads(Path('/build-contract/app-runtime.lock.json').read_text())
contract = lock['buildCompatibilityPatches']['ndn-svs-boost-1.71']
patch = Path('/build-contract/ndn-svs-boost-1.71.patch')
observed = hashlib.sha256(patch.read_bytes()).hexdigest()
if observed != contract['sha256']:
    raise SystemExit('NDN_SVS_BOOST_PATCH_DIGEST_MISMATCH')
PY
RUN set -eu; cd /src/ndn-svs; \
    test "$(grep -Fc 'BOOST_VERSION_NUMBER < 107400' wscript)" -eq 1; \
    test "$(grep -Fc 'minimum supported version of Boost is 1.74.0' wscript)" -eq 1; \
    patch --batch --forward --fuzz=0 -p1 < /build-contract/ndn-svs-boost-1.71.patch; \
    test "$(grep -Fc 'BOOST_VERSION_NUMBER < 107100' wscript)" -eq 1; \
    test "$(grep -Fc 'minimum supported version of Boost is 1.71.0' wscript)" -eq 1; \
    test "$(grep -Fc 'BOOST_VERSION_NUMBER < 107400' wscript)" -eq 0; \
    test "$(grep -Fc 'minimum supported version of Boost is 1.74.0' wscript)" -eq 0

RUN set -eu; cd /src/ndn-svs; \
    ./waf configure --prefix=$APP_PREFIX; \
    ./waf -j"$BUILD_JOBS"; ./waf install
RUN set -eu; cd /src/NDNSD; \
    ./waf configure --prefix=$APP_PREFIX; \
    ./waf -j"$BUILD_JOBS"; ./waf install
RUN set -eu; cd /src/ndnsf; \
    ./waf configure --prefix=$APP_PREFIX --with-examples; \
    ./waf -j"$BUILD_JOBS" \
      --targets=ndn-service-framework,libndn-service-framework.pc,App_ServiceController,di-native-provider; \
    install -d $APP_PREFIX/bin $APP_PREFIX/lib $APP_PREFIX/lib/pkgconfig; \
    install -m 0755 build/libndn-service-framework.so \
      $APP_PREFIX/lib/libndn-service-framework.so.0.1.0; \
    ln -s libndn-service-framework.so.0.1.0 \
      $APP_PREFIX/lib/libndn-service-framework.so; \
    install -m 0644 build/libndn-service-framework.pc \
      $APP_PREFIX/lib/pkgconfig/libndn-service-framework.pc; \
    install -m 0755 build/examples/App_ServiceController $APP_PREFIX/bin/App_ServiceController; \
    install -m 0755 build/examples/di-native-provider $APP_PREFIX/bin/di-native-provider
RUN python3 - <<'PY' >/tmp/owner-profiles
import json
print(' '.join(json.load(open('/build-contract/app-runtime.lock.json'))['ownerProfiles']))
PY
RUN python3 - <<'PY' >/tmp/app-runtime-python-requirements
import json
lock = json.load(open('/build-contract/app-runtime.lock.json'))
for name, version in sorted(lock.get('runtimePythonPackages', {}).items()):
    print(f'{name}=={version}')
PY
RUN set -eu; cd /src/ndnsf; \
    install -d $APP_PREFIX/python; \
    profile_paths=""; \
    for profile in $(cat /tmp/owner-profiles); do \
      profile_paths="$profile_paths ./NDNSF-DistributedInference/packaging/python/$profile"; \
    done; \
    if [ -s /tmp/app-runtime-python-requirements ]; then \
      /opt/venv/bin/pip install --no-deps --upgrade --target=$APP_PREFIX/python \
        --requirement /tmp/app-runtime-python-requirements; \
    fi; \
    CPLUS_INCLUDE_PATH=$APP_PREFIX/include:/opt/ndn-base/include \
    NDNSF_LIBRARY_DIR=$APP_PREFIX/lib \
    /opt/venv/bin/pip install --no-deps --upgrade --target=$APP_PREFIX/python \
      ./pythonWrapper ./NDNSF-DistributedRepo/pythonWrapper $profile_paths
ARG APP_BUILD_ID
RUN install -d $APP_PREFIX/manifest && \
    install -m 0644 /build-contract/app-runtime.lock.json $APP_PREFIX/manifest/ && \
    install -m 0644 /build-contract/seal/seal.json $APP_PREFIX/manifest/source-seal.json && \
    printf '%s\n' "$APP_LOCK_DIGEST" >$APP_PREFIX/manifest/lock-digest && \
    printf '%s\n' "$APP_SEAL_DIGEST" >$APP_PREFIX/manifest/seal-digest && \
    printf '%s\n' "$APP_BUILD_ID" >$APP_PREFIX/manifest/app-build-id && \
    python3 /build-contract/derive-runtime-packages.py \
      --root $APP_PREFIX --output $APP_PREFIX/manifest/runtime-system-packages && \
    test -x $APP_PREFIX/bin/App_ServiceController && \
    test -x $APP_PREFIX/bin/di-native-provider && \
    ldd $APP_PREFIX/bin/di-native-provider | grep -F '/opt/onnxruntime/lib/libonnxruntime.so' && \
    PYTHONPATH=$APP_PREFIX/python /opt/venv/bin/python -c \
      'import ndnsf,ndnsf_distributed_inference,ndnsf_distributed_inference.core,ndnsf_distributed_inference.sdk,ndnsf_distributed_inference.app_sdk,ndnsf_distributed_inference.app_sdk.provider,ndnsf_distributed_inference.app_sdk.client,ndnsf_distributed_inference.app_sdk.controller,ndnsf_distributed_inference.deployment,ndnsf_distributed_inference.retry,ndnsf_distributed_inference.runtime_v1,ndnsf_distributed_inference.runtime_v1_evidence,ndnsf_distributed_inference.planner,ndnsf_distributed_inference.ops,ndnsf_distributed_inference.adapters.onnx,ndnsf_distributed_inference.adapters.qwen'

FROM ${NDN_RUNTIME_IMAGE} AS app-runtime
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
ARG APP_LOCK_DIGEST
ARG APP_SEAL_DIGEST
ARG APP_BUILD_ID
ARG DEBIAN_FRONTEND=noninteractive
COPY --from=app-builder /opt/ndnsf-app/manifest/runtime-system-packages /tmp/runtime-system-packages
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    missing_packages="$(while IFS= read -r package; do \
      [ -n "$package" ] || continue; \
      if ! dpkg-query -W -f='${db:Status-Abbrev}' "$package" 2>/dev/null | grep -qx '.i '; then \
        printf '%s\n' "$package"; \
      fi; \
    done < /tmp/runtime-system-packages)" && \
    if [ -n "$missing_packages" ]; then \
      sed -i 's|http://archive.ubuntu.com|https://archive.ubuntu.com|g; s|http://security.ubuntu.com|https://security.ubuntu.com|g' /etc/apt/sources.list && \
      rm -rf /var/lib/apt/lists/* /var/cache/apt/archives/*.deb /var/cache/apt/archives/partial/* && \
      apt-get -o Acquire::Retries=3 update && \
      apt-get install -y --no-install-recommends $missing_packages; \
    fi && \
    rm -rf /var/lib/apt/lists/* /tmp/runtime-system-packages
COPY --from=app-builder /opt/ndnsf-app /opt/ndnsf-app
COPY packaging/ndnsf-di-container/oci/compatibility/gpu-matrix.yaml /opt/ndnsf-app/manifest/gpu-matrix.yaml
COPY packaging/ndnsf-di-container/oci/scripts/entrypoint.sh /usr/local/bin/ndnsf-di-container-entrypoint
COPY packaging/ndnsf-di-container/oci/scripts/healthcheck.sh /usr/local/bin/ndnsf-di-container-healthcheck
COPY packaging/ndnsf-di-container/oci/scripts/probe-runtime.py /usr/local/bin/ndnsf-di-probe-runtime
COPY packaging/ndnsf-di-container/oci/scripts/verify-runtime-closure.py /usr/local/bin/verify-runtime-closure.py
COPY packaging/ndnsf-di-container/lib/gpu_compatibility.py /usr/local/lib/ndnsf-di/gpu_compatibility.py
COPY packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-ndnsf-qwen.sh /opt/ndnsf/bin/run-ndnsf-qwen.sh
COPY packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-qwen-reference.py /opt/ndnsf/bin/run-qwen-reference.py
COPY packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/sample-qwen-resources.py /opt/ndnsf/bin/sample-qwen-resources.py
RUN for test_data in \
      /opt/venv/lib/python3.10/site-packages/onnx/backend/test/data \
      /opt/venv/lib/python3.10/site-packages/onnxruntime/datasets; do \
      if [ -d "$test_data" ]; then \
        find "$test_data" -depth -delete; \
      fi; \
    done
RUN chmod 0755 /usr/local/bin/ndnsf-di-container-entrypoint \
      /usr/local/bin/ndnsf-di-container-healthcheck \
      /usr/local/bin/ndnsf-di-probe-runtime \
      /usr/local/bin/verify-runtime-closure.py \
      /opt/ndnsf/bin/run-ndnsf-qwen.sh \
      /opt/ndnsf/bin/run-qwen-reference.py \
      /opt/ndnsf/bin/sample-qwen-resources.py && \
    install -d -m 0755 /etc/ndn && \
    install -m 0644 /opt/ndn-base/manifest/nfd.conf /etc/ndn/nfd.conf && \
    install -d -m 0750 -o 65532 -g 65532 \
      /run/nfd /run/ndnsf-di /var/lib/ndnsf-di /tmp/ndnsf-di && \
    ldconfig
ENV PATH=/opt/venv/bin:/opt/ndnsf-app/bin:/opt/ndn-base/bin:${PATH} \
    LD_LIBRARY_PATH=/opt/ndnsf-app/lib:/opt/ndn-base/lib:/opt/onnxruntime/lib \
    PKG_CONFIG_PATH=/opt/ndnsf-app/lib/pkgconfig:/opt/ndn-base/lib/pkgconfig:/opt/onnxruntime/lib/pkgconfig \
    PYTHONPATH=/opt/ndnsf-app/python \
    PYTHONNOUSERSITE=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    HOME=/run/ndnsf-di \
    NDNSF_RELEASE_ROOT=/opt/ndnsf-app \
    NDNSF_BACKEND=onnxruntime-cuda \
    NDNSF_ALLOW_CPU_FALLBACK=0
RUN python3 /usr/local/bin/verify-runtime-closure.py --root /opt/ndnsf-app && \
    /opt/venv/bin/python -c \
      'import ndnsf,ndnsf_distributed_inference,ndnsf_distributed_inference.core,ndnsf_distributed_inference.sdk,ndnsf_distributed_inference.app_sdk,ndnsf_distributed_inference.app_sdk.provider,ndnsf_distributed_inference.app_sdk.client,ndnsf_distributed_inference.app_sdk.controller,ndnsf_distributed_inference.deployment,ndnsf_distributed_inference.retry,ndnsf_distributed_inference.runtime_v1,ndnsf_distributed_inference.runtime_v1_evidence,ndnsf_distributed_inference.planner,ndnsf_distributed_inference.ops,ndnsf_distributed_inference.adapters.onnx,ndnsf_distributed_inference.adapters.qwen,torch,onnxruntime,transformers' && \
    test ! -e /src && test ! -e /root/.ssh && test ! -e /root/.cache/huggingface
LABEL org.opencontainers.image.title="NDNSF-DI layered local GPU runtime" \
      org.ndnsf.di.layer="app-runtime" \
      org.ndnsf.di.app-lock="${APP_LOCK_DIGEST}" \
      org.ndnsf.di.app-seal="${APP_SEAL_DIGEST}" \
      org.ndnsf.di.app-build-id="${APP_BUILD_ID}" \
      org.ndnsf.di.backend="onnxruntime-cuda" \
      org.ndnsf.di.rootfs="read-only" \
      org.ndnsf.di.models-included="false"
USER 65532:65532
WORKDIR /run/ndnsf-di
RUN /usr/local/bin/ndnsf-di-probe-runtime --mode static
HEALTHCHECK --interval=30s --timeout=10s --retries=3 \
  CMD ["/usr/local/bin/ndnsf-di-probe-runtime", "--mode", "static"]
ENTRYPOINT ["/usr/local/bin/ndnsf-di-container-entrypoint"]
CMD ["exec", "/usr/local/bin/ndnsf-di-probe-runtime", "--mode", "static"]
