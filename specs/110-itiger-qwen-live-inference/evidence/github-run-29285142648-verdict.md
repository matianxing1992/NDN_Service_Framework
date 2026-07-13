# GitHub OCI run 29285142648 verdict

**Date**: 2026-07-13  
**Task**: T162  
**Source revision**: `a76bd46ca4d46de44dee47329a67d6ab98833703`  
**Run**: `https://github.com/matianxing1992/NDN_Service_Framework/actions/runs/29285142648`  
**Verdict**: `EXECUTED_FAIL`; never rerun this run/release identity

## Preserved execution result

The push-triggered job ran exactly once and completed `failure` after 6m36s.
The public check annotation identified the separately isolated NFD layer:

```text
buildx failed with: ERROR: failed to build: failed to solve: process
"/bin/sh -c set -eu; cd /src/dependencies/NFD; ./waf configure
--prefix=$PREFIX; ./waf -j\"$(nproc)\"; ./waf install"
did not complete successfully: exit code: 1
```

This establishes that the T161 order repair passed the preceding independent
`ndn-cxx`, `ndn-svs`, and `NDNSD` layers and moved the failure boundary to NFD.
The run retained two evidence-only artifacts:

- Buildx record: 124,048 bytes, artifact ID `8293004261`, digest
  `sha256:34bb8ba6a53f4700417a88a148e95c4d04331251abbcc27a423c45bc5eb0879f`;
- release evidence: 2,042 bytes, artifact ID `8293003547`, digest
  `sha256:6d652b03e673f86302c31df0a029229453554024fd5603e80d6e5073152badd4`.

Neither artifact is an OCI image. No GHCR release digest, manifest, signature,
SBOM, SIF, Slurm submission, GPU result, or Qwen result was created.

## Root cause and local proof

The locked NFD 24.07 build contract enables Ethernet and WebSocket faces by
default. The sealed GPU lock omitted both inputs required by those defaults:

1. `libpcap-dev` was absent from `systemPackages`, so NFD configuration stopped
   at its mandatory `libpcap` check;
2. the exact NFD Git archive contains only the `websocketpp` gitlink and not the
   submodule contents, while NFD requires `websocketpp/websocketpp/version.hpp`
   inside its source tree.

A disposable Ubuntu 22.04 container used the exact locked ndn-cxx and NFD
commits plus NFD's exact websocketpp gitlink commit. Before installing
`libpcap-dev`, it reproduced:

```text
NFD_CONFIGURE_WITHOUT_LIBPCAP_RC=1
Checking for libpcap library : not found, but required for Ethernet face support.
The configuration failed
```

After installing `libpcap-dev` and materializing websocketpp commit
`ac4e021333675fc80b96eb7be45d218581c897e2`, the same container completed:

```text
NFD_CONFIGURE_AFTER_LIBPCAP=PASS
NFD_BUILD_INSTALL=PASS
24.07
```

The 12 MiB retained diagnostic bundle has log digest
`sha256:ae671d118d5bcd6c7c56dd3c8fcc20ba4b2629a4361187e9d8bcf1c0ea3ccddb`.
The disposable container and Ubuntu image were removed; root free space
returned to 26 GiB.

## Replacement boundary

The repair adds `libpcap-dev` to the immutable system-package lock and adds the
NFD gitlink's exact websocketpp revision as a seventh sealed source archive.
The Dockerfile verifies and materializes that archive inside NFD before
configuration. The failed run remains frozen:

```text
run29285142648=EXECUTED_FAIL
failureClass=NFD_SEALED_BUILD_INPUTS_INCOMPLETE
runnerDiskExhaustion=NOT_OBSERVED
ghcrDigest=NOT_AVAILABLE
runtimeSif=NOT_AVAILABLE
slurmSubmission=NOT_EXECUTED
replacementTask=T164_NEW_SOURCE_REVISION
```
