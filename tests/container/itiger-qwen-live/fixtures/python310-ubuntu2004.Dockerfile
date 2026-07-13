ARG PYTHON_BASE_IMAGE=python@sha256:b3061b93c8df9809c3783a4f17bbf2520425ec6b40bd3e5e7538870e21ba7209
ARG RUNTIME_BASE_IMAGE=ubuntu@sha256:8feb4d8ca5354def3d8fce243717141ce31e2c428701f6682bd2fafe15388214
FROM ${PYTHON_BASE_IMAGE} AS python-runtime
RUN rm -f \
      /usr/local/lib/python3.10/lib-dynload/nis.*.so \
      /usr/local/lib/python3.10/lib-dynload/_tkinter.*.so
FROM ${RUNTIME_BASE_IMAGE} AS python-copied
RUN apt-get update && apt-get install -y --no-install-recommends \
      libgdbm6 libreadline8 libsqlite3-0 libssl1.1 && \
    rm -rf /var/lib/apt/lists/*
COPY --from=python-runtime /usr/local /usr/local
RUN ldconfig
FROM python-copied AS verified
RUN python3.10 --version && \
    python3.10 -c 'import bz2,ctypes,decimal,hashlib,lzma,readline,sqlite3,ssl,sys,uuid; assert sys.version_info[:2] == (3, 10); assert ssl.OPENSSL_VERSION.startswith("OpenSSL 1.1.1f"); print(ssl.OPENSSL_VERSION); ctypes.CDLL("libc.so.6")' && \
    test -z "$(find /usr/local/lib/python3.10/lib-dynload -type f \( -name 'nis.*.so' -o -name '_tkinter.*.so' \) -print)" && \
    find /usr/local -type f -name '*.so' -exec sh -c \
      'for f do if ldd "$f" 2>/dev/null | grep -q "not found"; then echo "MISSING:$f"; exit 1; fi; done' sh {} +
