# Local SIF build route correction — 2026-08-17

The release process starts with a complete application SIF built on the local
host with Apptainer 1.3.4, matching TigerCluster's `1.3.4-1.el9`. Tiger only
verifies the promoted file's hash and executes it; it does not build, download,
or materialize the application image.

The local build has one release contract: the SIF is the only promoted runtime
artifact. The current Spec170 candidate was built directly with host Apptainer
from the sealed source and a qualified local base SIF (`localimage` refresh),
then checked before upload. There was no Docker/Buildah step. No OCI archive,
`docker://` pull, or Tiger-side build is part of the current route; any older
OCI/Docker material is historical provenance only and is not a release input.

An Apptainer `pull docker://...` of a base image is neither route: it only
materializes an unrelated base image and does not build NDNSF-DI. It must not be
used as a replacement for the application build or promoted as a candidate.

The latest submitted candidate is the r4 role-evidence SIF below. The older
multi-device and Dockerfile/OCI candidates are retained only as historical
provenance and must not be selected for new Spec170 evidence.

## Latest submitted candidate (r4)

```text
release:       spec170-runtime-edd1e096-role-evidence-python-20260817-r4
source:        edd1e0965e688f996fb5f37ce80043ce67aa19ed
source seal:   sha256:706a39a308198396cc74f056b62e8c5dfed18483111ecd048642b7d1c19f4141
SIF path:      /tmp/spec170-release-edd1e096-role-evidence-python-20260817-r4/runtime.sif
SIF SHA:       sha256:585edf7805c1ffd57e72053fdbecf832da1730c70b0dc1def1caa86517ed9926
SIF bytes:     4397608960
provider SHA:  sha256:cd3f69615662c1fe40a61b63f0589c39c3b5810c546946064b2c20896f861d68
python SHA:    sha256:78b4cd6a1669b25089153659e027fe5700709cf42137cc9602629e96c6afe5d3
build:         host Apptainer 1.3.4 localimage refresh
Tiger action:  verify hash and execute only
```

The r4 SIF passed import, provider hash, CUDA/ONNX Runtime readiness, and
`ldd` resolution, but the new fail-closed internal-library checker found
build-host `/tmp` and checkout RPATH/RUNPATH entries. It is
`BLOCKED_LOCAL_LIBRARY_CLOSURE`; the negative audit is
`spec170-sif-library-closure-r4-fail-20260817.md`. A fresh release-only-RPATH
SIF is required before another Tiger job.

## Historical multi-device-evidence SIF candidate

```text
release:       spec170-runtime-52ad67fd-multidevice-20260817-r1
source:        52ad67fdfcd10b21e5c467bada86b3dca0e96428
source seal:   sha256:1762f19ad6b229c62b70504af8e2b10c5360b9af28937f8e03456afb6f34b998
SIF path:      /tmp/spec170-release-52ad67fd-multidevice-20260817/runtime.sif
SIF SHA:       sha256:3fa4bc6d411e8b19ebaabd63879e584b405c7ab201a660f19b19d64d3353b68e
SIF bytes:     4398845952
provider SHA:  sha256:63f223ec2ccd8621bdbfe465a453dc2fe03df884de1a267a877bc2c37f05b283
build:         host Apptainer 1.3.4 localimage refresh
Tiger action:  verify hash and execute only
```

The source adds backward-compatible `device.ids`/`gpuUuids` arrays and allows
one Provider's aggregate evidence to represent roles that ran on multiple CUDA
devices; single-device evidence keeps the existing scalar fields. This older
candidate is historical; it must not be used to qualify a new Tiger run.

## Active post-fix local SIF candidate

```text
release:       spec170-runtime-dd5c11cc-localfix-20260817-r1
source:        dd5c11cc6cf7cef09c3f4a1bc1bf8d215d61962c
source seal:   sha256:110745a2d7c3676af952d5a27e2302a0cfe2f96cbe2714791e6d1881453bee24
SIF path:      /tmp/spec170-runtime-dd5c11cc-localfix-20260817-r1.sif
SIF SHA:       sha256:f6521a8226190279cb961f2e100245d783c8ba90723a4b004f1f773df71b5874
SIF bytes:     4398788608
provider SHA:  sha256:3dcfaa94fabfaebff967c191c7abea3b9d69be9360267a49f72167f216d2a6d4
build:         host Apptainer 1.3.4 localimage refresh
Tiger action:  verify hash and execute only
```

Local static/import/runner-help/`ldd` closure checks passed. D1 and D2 remain
runtime gates; this record is not a claim that the network workload has passed.

## Historical evidence

Older OCI, Docker, foundation-only, alternate-version, and Tiger-side build
attempts remain in the dated build-run and cleanup records for auditability.
They are not release inputs. Do not copy their commands, hashes, or image
names into a new Spec170 run; start again from a newly sealed source tree and a
new local application SIF.

The current route is intentionally documented in one place: the Spec170
`quickstart.md` release-construction section. This file records only the
candidate identities and local verification results; Tiger acceptance belongs
in the per-job evidence files.
