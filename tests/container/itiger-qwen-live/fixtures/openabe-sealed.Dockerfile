FROM ubuntu@sha256:8feb4d8ca5354def3d8fce243717141ce31e2c428701f6682bd2fafe15388214
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      bison build-essential ca-certificates cmake flex libgmp-dev libgtest-dev libssl-dev python3 && \
    rm -rf /var/lib/apt/lists/*
RUN apt-get update && apt-get install -y --no-install-recommends libfl-dev && \
    rm -rf /var/lib/apt/lists/*
COPY openabe /src/openabe
COPY relic /src/relic
COPY prepare-openabe-relic.py /usr/local/bin/prepare-openabe-relic.py
RUN python3 /usr/local/bin/prepare-openabe-relic.py --source /src/relic --openabe /src/openabe
RUN set -eu; cd /src/openabe; \
    export LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}; \
    . ./env; \
    make -C deps/relic relic-toolkit-0.5.0/.built; \
    NO_DEPS=1 BISON="$(command -v bison)" make; \
    make test; \
    make INSTALL_PREFIX=/opt/openabe install; \
    test -f /opt/openabe/lib/libopenabe.so; \
    test -f /opt/openabe/lib/librelic.so
