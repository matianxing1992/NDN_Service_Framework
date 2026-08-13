# Spec170 Gate A in-process integration checkpoint — 2026-08-13

## Candidate inputs

- NDNSF source: `Experimental@1104353861b0c223b8445e49af9b00028121ad1f`
- ndn-svs source: `spec170-runtime-v3@060811333de68b9674e45522222a14d4e047bf28`
- ndn-cxx prefix: `/home/tianxing/NDN/ndn-cxx/build`
- ndn-svs prefix: `/tmp/ndn-svs-spec170-prefix`
- Compiler: `/usr/bin/g++` 9.4.0
- Linker: `/usr/bin/ld` (system binutils; no Homebrew linker)
- Boost: system 1.71.0

## Commands and results

The corrected ndn-svs candidate was configured, built, installed, and tested
before compiling NDNSF:

```text
PKG_CONFIG_PATH=/home/tianxing/NDN/ndn-cxx/build \
  /tmp/ndn-svs-spec170-latest-20260813/waf configure --with-tests
PATH=/usr/bin:/bin:$PATH PKG_CONFIG_PATH=/home/tianxing/NDN/ndn-cxx/build \
  /tmp/ndn-svs-spec170-latest-20260813/waf -j$(nproc)
LD_LIBRARY_PATH=/tmp/ndn-svs-spec170-latest-20260813/build:... \
  /tmp/ndn-svs-spec170-latest-20260813/build/unit-tests -l test_suite -x
```

The full ndn-svs binary reported **82 test cases, no errors detected**.

NDNSF was then configured against the corrected prefix and built with the
integration target:

```text
PATH=/usr/bin:/bin:$PATH \
PKG_CONFIG_PATH=/tmp/ndn-svs-spec170-prefix/lib/pkgconfig:/home/tianxing/NDN/ndn-cxx/build:/home/tianxing/NDN/NDNSD/build:/home/tianxing/NDN/NFD/build:/home/tianxing/NDN/NAC-ABE/build \
  ./waf configure --with-tests
PATH=/usr/bin:/bin:$PATH \
PKG_CONFIG_PATH=/tmp/ndn-svs-spec170-prefix/lib/pkgconfig:/home/tianxing/NDN/ndn-cxx/build:/home/tianxing/NDN/NDNSD/build:/home/tianxing/NDN/NFD/build:/home/tianxing/NDN/NAC-ABE/build \
  ./waf build --targets=integration-tests -j$(nproc)
```

The target linked successfully in 2m03.656s. Runtime verification used the
same corrected SVS prefix and `/tmp/ndnsf-experimental-latest/examples/trust-any.conf`:

```text
./build/integration-tests --run_test='NdnSvsSmoke/*'                 # 2/2 PASS
./build/integration-tests --run_test='Spec170NdnsfDiCoreFlow/*'     # 9/9 PASS
PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper \
  python3 -m pytest -q tests/python/test_spec170_integrated_flows.py  # 6 PASS
```

The Spec170 C++ flow emitted READY and request lifecycle evidence for the
normal, drop, duplicate, reorder, three-provider, and custom-selection cases;
each request ended with `RESET`. The run also emitted the expected cancellation
and deadline terminal records. Python integrated flows passed all six cases.

Recorded artifact SHA-256 values:

```text
integration-tests: 767d043fef6574ea3d6e7d35d96a36f507c7cdd9ec5fe77c6789c14f93bda6c3
ndn-svs unit-tests: 8a78eb38eefbc1eaedbe58f006c80551d2851ba21e6fbc3b29b3468d0640567c
libndn-svs.so.0.1.0: a3e414c263515edf8ac7c7d73d258635b1715f69836f0325f28ccc0cc339cf87
gpu.lock: 710953c746bbae727f4a33ab0d6a7fb3a3810ce9956f5d71dbed9f131bc72d3e
```

This is a Gate A in-process integration checkpoint, not Gate A closure: the
complete 120-vector/mutation corpus and every contract row still require the
declared Gate A command and evidence aggregation. It does establish that the
current NDNSF V3 source can compile and execute against the corrected SVS
runtime, and it removes the old-SVS-prefix ambiguity from subsequent gates.
