# T011 r42 Python-extension build boundary

Date: 2026-09-05 (local)

## Disposition

`CLOSED as command-invocation error`.

The first extension rebuild attempt used:

```text
NDNSF_LIBRARY_DIR=/home/tianxing/NDN/ndn-service-framework/build-system-j2 python3 pythonWrapper/setup.py build_ext --inplace --force
```

from the repository root. The repository's `pythonWrapper/setup.py` declares
the relative source `src/ndnsf/_ndnsf.cpp`; setuptools therefore invoked the
compiler from the repository root and stopped before compilation with:

```text
gcc: error: src/ndnsf/_ndnsf.cpp: No such file or directory
```

The command exited 1 and produced no extension artifact. This closes as a
build-command boundary only; it does not change the source or candidate
identity and does not provide protocol evidence.

## Closure and next action

Retry from `pythonWrapper/` while keeping `NDNSF_LIBRARY_DIR` as the absolute
current `build-system-j2` directory. No SIF or Tiger execution was performed.
