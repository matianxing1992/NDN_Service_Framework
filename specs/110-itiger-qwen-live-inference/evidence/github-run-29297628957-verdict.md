# GitHub GPU assembly run 29297628957 verdict

**Verdict**: `EXECUTED_FAIL`; this candidate identity is frozen and MUST NOT be
rerun. The failure is a GPU-assembler runtime-package ordering defect, not a
runner-disk failure and not GPU, SIF, Slurm, or Qwen execution evidence.

## Immutable identity

| Field | Value |
|---|---|
| Workflow run | `29297628957` (run 7, attempt 1) |
| URL | `https://github.com/matianxing1992/NDN_Service_Framework/actions/runs/29297628957` |
| Event | `workflow_dispatch` |
| Source revision | `edeeff1e3041e941b48ba18784348fc9505d7418` |
| Source ref | `spec110-gpu-source-edeeff1e3041e941b48ba18784348fc9505d7418` |
| Release identity | `spec110-runtime-edeeff1e3041e941b48ba18784348fc9505d7418` |
| Foundation input | `ghcr.io/matianxing1992/ndnsf-di-foundation@sha256:801e8cabc5e084347cb835f107bd9e4c36f07543827c078f8b4720cbf1b48df8` |
| Dispatch accepted | `2026-07-14T01:04:23Z`, HTTP 204 |
| Terminal state | `completed/failure`, `2026-07-14T01:24:30Z` |
| Retry policy | `NO_AUTOMATIC_RERUN`; no retry occurred |

The final pre-dispatch review proved that the source tag resolved exactly to
the committed source revision, the Foundation digest was anonymously readable,
the workflow was active, and there was no existing run for this release
identity. The workflow dispatch API was called exactly once.

## Measured failure

All 244 NDNSF workspace/GPU-native build actions completed. The next
`gpu-assembler` closure step failed with:

```text
RuntimeError: RUNTIME_LIBRARY_MISSING:/opt/ndnsf-di/current/bin/nfd
```

`derive-runtime-packages.py` deliberately rejects any `ldd` output containing
`not found`. The accepted Foundation generated
`/opt/ndnsf-di/manifest/runtime-system-packages`, including `libpcap0.8`,
Boost 1.71 runtime libraries, OpenSSL 1.1, SQLite, GMP, libgomp, libstdc++,
Python 3, zlib, and CA certificates. Foundation NFD links
`libpcap.so.0.8`. The GPU assembler copied that manifest but did not install it
before invoking the `ldd`-based closure scan. The final runtime stage installed
the manifest only after the failed assembler stage, which was too late.

The resolver warning concerning `/etc/resolv.conf` is non-controlling because
the build continued through dependency installation and all 244 workspace
actions. The runner had 89 GB free before the build and 62 GB free afterward;
disk capacity is therefore excluded as the cause. Docker reported a 24.77 GB
active local volume after the failure, but the filesystem still had ample free
space.

No final GPU OCI digest, release manifest, SBOM, signature, SIF, Slurm job,
CUDA-provider observation, or Qwen inference result was produced. This run
cannot support any cloud GPU or distributed-inference claim.

## Preserved evidence

The complete local evidence copy is under the ignored experiment directory
`results/spec110-itiger-qwen-live/github-dispatch-t167/`.

| Artifact | Identity and digest |
|---|---|
| Complete run logs | `run-logs.zip`, 174730 bytes, `sha256:76420dade9cef16bf7828d297ba1a73c84d61c80684b230807e19a5ff15b6468` |
| Buildx record | artifact `8297630200`, 128937 bytes, `sha256:01a436a8f07c3a4a93ded22ce67221b480c07f96a214ac621f4b1318151c94c9` |
| Release evidence | artifact `8297629836`, 1828 bytes, `sha256:15d29a2ef03622b468dec0adb9130e6625b3ddaed3510cd74c8f2f8d3a1d9184` |

The release-evidence artifact contains the preflight result, source secret scan,
build-context identity, and runner disk reports. The preflight and secret scan
passed, but the preflight was incomplete because it checked for the closure
derivation marker without checking that Foundation runtime packages were
installed before that marker.

## Replacement boundary

The repair installs the Foundation-measured runtime package manifest in the GPU
assembler before any `ldd` closure scan. A new unit regression and preflight
ordering invariant fail when that install is absent or late. The original RED
test failed on the accepted source exactly because the marker was absent; the
repaired focused suite and preflight then passed.

The final T168 local gate recorded:

- 98/98 Spec 110 offline unit/contract tests passed with zero failure, error,
  or skip; JUnit evidence is retained at
  `results/spec110-itiger-qwen-live/offline-foundation/junit.xml`;
- direct GPU-build preflight passed with eight locked sources, 40 Python
  packages, and 11 system CUDA requirements;
- the release-pipeline integration ended in `RELEASE_PIPELINE_PASS`;
- the six changed source/document files scanned 83002 bytes with zero secret
  findings;
- Python compilation and `git diff --check` passed; and
- strict Spec Kit structure audit passed with 37 functional requirements, 13
  success criteria, six user stories, 171 tasks, and complete requirement
  traceability.

The first wrapper invocation used the existing evidence directory itself as
`--output`. All 98 tests passed, but JUnit serialization correctly failed with
`IsADirectoryError`. The accepted invocation names
`offline-foundation/junit.xml`; this was a local evidence-path correction, not
a cloud candidate rerun.

This correction changes the workspace source identity. It therefore requires a
new committed revision, source seal, source-bound Foundation tag and immutable
Foundation digest, followed by a newly named release and one separately
authorized workflow dispatch. Run `29297628957`, its source tag, and its release
identity remain immutable negative evidence.
