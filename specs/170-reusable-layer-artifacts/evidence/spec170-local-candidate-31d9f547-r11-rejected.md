# Spec170 local candidate r11 rejected (2026-08-17)

Candidate r11 completed local Apptainer construction but is not a promotable
release. Its content SHA-256 was
`d2794b62f536ed8b3dcad4bf916ac3db0023928ba4546be537de0c20f812031b`
(4,398,850,048 bytes), while `apptainer inspect` retained the base image label
`org.ndnsf.di.release=spec170-runtime-31d9f547-post-selection-v8-20260817`
instead of the definition's intended v9 identity.

The original build record is retained verbatim with SHA-256
`42cff68079fd1bfde3cac0cafc5b0e53b1672f77d70c337aa139185777b32987`.
Its internal `status=PASS` reflects only the pre-fix builder result and is
superseded by this release-identity rejection. Candidate r11 was never uploaded
or submitted to TigerCluster.

Root cause: Apptainer 1.3.4 does not overwrite an inherited localimage label
unless `apptainer build --force` is used. A minimal scratch/localimage
experiment reproduced this behavior. The local-SIF builder now uses `--force`
and verifies every `org.ndnsf.di.*` label through `apptainer inspect` before it
moves the partial SIF or writes a PASS record.
