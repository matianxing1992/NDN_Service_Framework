# GitHub OCI run 29283330479 verdict

**Date**: 2026-07-13  
**Task**: T159  
**Source revision**: `271a152b25d3efeb41452434c817193f48243fc8`  
**Run**: `https://github.com/matianxing1992/NDN_Service_Framework/actions/runs/29283330479`  
**Verdict**: `EXECUTED_FAIL`; never rerun this run/release identity

## Preserved execution result

The push-triggered job started once on GitHub-hosted runner
`GitHub Actions 1000000071` and completed `failure`. These steps passed before
the failure:

- checkout and immutable-input validation;
- release evidence directory and runner-disk capture;
- creation and verification of all sealed dependency archives;
- source secret scan;
- Buildx setup and GHCR login.

The only failed step was `Build and publish immutable GPU image`. The public
check-run annotation was:

```text
buildx failed with: ERROR: failed to build: failed to solve: process
"/bin/sh -c set -eu; for project in ndn-cxx NDNSD ndn-svs; do ..."
did not complete successfully: exit code: 1
```

The source/build evidence artifact was retained as
`spec110-release-build-spec110-runtime-271a152...` (artifact ID `8292499530`,
artifact digest
`sha256:8e02cdb4445e76d38852af483ed9d9ecf0ded397608ad59027f5418600308d4f`).
Buildx also retained its 120,748-byte build record (artifact ID `8292500321`,
digest
`sha256:9bb65c9f04353a08469e696031738084e9f45fb1c3039cae94b3d299a4626c8e`).
Neither artifact contains the OCI image. No successful manifest, signature,
SBOM, GHCR release digest, SIF, Slurm submission, GPU result, or Qwen result was
created.

## Root cause

This was not a runner-disk failure. The Dockerfile configured projects in the
order `ndn-cxx -> NDNSD -> ndn-svs`, while the exact locked NDNSD `wscript`
requires both `libndn-cxx >= 0.8.0` and `libndn-svs >= 0.1.0`. NDNSD therefore
ran before its ndn-svs prerequisite existed.

The Dockerfile now uses separate cache/error boundaries and the correct order:

```text
ndn-cxx -> ndn-svs -> NDNSD
```

A RED regression test first reproduced the old order, then passed after the
fix. The full Spec 110 offline suite passed 87/87.

## Minimal environment-matched reproduction

The pinned CUDA build image manifest identifies Ubuntu 22.04. A temporary
`ubuntu:22.04` container consumed the same locked Git archives and compiled the
corrected sequence without CUDA, PyTorch, final-image export, or host install:

```text
CONTAINER_BUILD_PASS ndn-cxx
CONTAINER_BUILD_PASS ndn-svs
CONTAINER_BUILD_PASS NDNSD
libndn-cxx=0.9.0
libndn-svs=0.1.0
SPEC110_UBUNTU2204_DEPENDENCY_ORDER_PASS
```

The 147,076-byte local diagnostic log digest was
`sha256:117edfe106ea5fa9c84c346ce05877651aff386ab4fea37c8853b1546b0dcd5f`.
A host-only Ubuntu 20.04 attempt stopped at ndn-svs because host Boost 1.71 is
below its 1.74 minimum; that result is an environment mismatch and is not used
as evidence for the Ubuntu 22.04 OCI base.

## Unrelated NFD notification

Publishing the exact NFD commit under `spec110-sealed-2b43d675e3fb` also
triggered that repository's existing CI/Docs matrices. NFD Docs run
`29282496210` failed only on macOS dependency installation because Homebrew now
requires explicit trust for `aws/tap`; its Ubuntu documentation jobs passed.
Those notifications do not change the commit's anonymous fetchability and are
not NDNSF-DI OCI results.

## Boundary and next identity

```text
run29283330479=EXECUTED_FAIL
failureClass=DEPENDENCY_BUILD_ORDER
runnerDiskExhaustion=NOT_OBSERVED
ghcrDigest=NOT_AVAILABLE
runtimeSif=NOT_AVAILABLE
slurmSubmission=NOT_EXECUTED
replacementTask=T162_NEW_SOURCE_REVISION
```

