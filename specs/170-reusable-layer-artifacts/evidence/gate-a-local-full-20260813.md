# Spec170 local Gate-A full regression checkpoint — 2026-08-13

This checkpoint records the current isolated source tree and corrected native
linkage after the Tiger OCI dependency fixes. It is local evidence only: it
does not close the deployment-faithful MiniNDN Gate B, exact-SIF Gate C, or the
T029 freeze.

## Candidate and linkage

- NDNSF source tree: `/tmp/ndnsf-spec170-fix.3pUFCo`
- source commit: `1cce2c305e04b44e23ff37a1bc0b225793e186aa`
- compiler path: `/usr/bin/g++` (system Boost 1.71)
- corrected ndn-svs source: `060811333de68b9674e45522222a14d4e047bf28`
- ndn-svs prefix: `/tmp/ndn-svs-spec170-prefix.C6UMXl`
- ndn-svs unit suite: 82 test cases, no errors detected (recorded in the
  earlier corrected-SVS checkpoint)
- `libndn-svs.so.0.1.0` SHA-256:
  `49812d807f6d2ff451de06c1e049c7a4e6204bcd1d1fc98f96ba1c518a93f41c`
- `gpu.lock` SHA-256:
  `cb3a5f6550676ec1a3d3afc024d1df5a7f51971997d50faa12d30317fd554fed`

`ldd build/unit-tests` resolved `libndn-svs.so.0.1.0` from the corrected
prefix and all listed Boost libraries from the system 1.71 installation.

## Commands and results

All commands used `PATH=/usr/bin:/bin` and:

```text
LD_LIBRARY_PATH=/tmp/ndn-svs-spec170-prefix.C6UMXl/lib:/tmp/ndnsf-spec170-fix.3pUFCo/build:/usr/local/lib
PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper
```

```text
python3 -m pytest -q tests/python/test_spec170_*.py
58 passed, 2 skipped, 1 warning in 5.46s

./build/unit-tests --log_level=message
*** No errors detected

./build/integration-tests --log_level=test_suite
11 test cases, *** No errors detected
```

The two Python skips are the explicitly gated real MiniNDN/Qwen environment
tests. The warning is the existing PyTorch `torch.load(weights_only=False)`
future warning. The native unit run also reported the expected unset-model
smoke skips; those are not a deployment pass.

Artifact hashes from this run:

```text
build/unit-tests: 86fca1edd96858a52d3784a9507df7c407d71864730d6507d45290b55a4e8507
build/integration-tests: 67d94aa23466981f0ccba1943f4a40faa49df0405defe8a8a30112f2bd6cf32e
build/libndn-service-framework.so.0.1.0: ea9d496be7e10435fcc909bf7fcf4423dac88eb36da1e59590257229bba158df
```

The immutable container/job rendering checks also passed independently:

```text
python3 -m pytest -q \
  tests/container/unit/test_spec170_allocation_topology.py \
  tests/container/unit/test_spec170_exact_sif_gate.py \
  tests/container/unit/test_slurm_render.py \
  tests/container/unit/test_slurm_submit.py \
  tests/container/unit/test_slurm_node_scripts.py
13 passed in 0.21s
```

## Tiger status

VPN and batch SSH are healthy. The accepted GPU OCI release remains pinned to
`ghcr.io/matianxing1992/ndnsf-di-spec170@sha256:94ce0cc847d453df90fc1aab74fade597f45e3199274ad782094fb45dd9bf916`.
The project release directory still rejects even a one-byte write with
`Disk quota exceeded`; therefore no new large SIF job was submitted and no
SIF/record is accepted by this checkpoint.

The next authorized step is one bounded node-local-scratch SIF build after the
storage quota is restored or an approved durable promotion path is provided.
