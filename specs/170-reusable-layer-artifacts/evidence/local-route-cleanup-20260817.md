# Local route cleanup — 2026-08-17

The normal Spec170 release path now starts with a locally built and locally
verified application SIF. The current candidate was built with host Apptainer
from the sealed source and a qualified local base SIF; no Docker/Buildah step
was used. TigerCluster only verifies the promoted SHA-256 and executes that
SIF. Docker/Buildah/OCI conversion is not part of the current release route;
older material is retained only as historical provenance.

To prevent the old remote-materialization route from being selected by mistake,
all explicitly identified temporary route materials were consolidated (not
deleted) in the one recoverable user-trash directory
`~/.local/share/Trash/files/ndnsf-spec170-route-cleanup-20260817/`:

- old materialization and Buildah job wrappers;
- the alternate-version runtime definition;
- stale build job-id markers; and
- zero-byte temporary foundation/release archives; and
- the duplicate `/tmp` localimage build metadata (the repository build record
  is authoritative); and
- the superseded pre-fix and pre-RPATH SIFs.

The superseded local SIFs were moved (not deleted) to the recoverable trash
directory above, so they cannot be mistaken for the active candidate:

```text
/tmp/spec170-runtime-8beaa9cd-r3.sif
sha256:4e5e56ef4893eb6f4f4e87c8b9d8f2c62621f3f04df8965ae4323545fa46d816
/tmp/spec170-runtime-8beaa9cd-r3.json
```

The pre-RPATH intermediate was also moved there and was never promoted:

```text
/tmp/spec170-runtime-dd5c11cc-localfix-20260817-r1-pre-rpath.sif
```

The active local candidate is recorded in
`spec170-local-sif-dd5c11cc-20260817.json` and has SIF SHA-256
`f6521a8226190279cb961f2e100245d783c8ba90723a4b004f1f773df71b5874`.

No project release, source seal, model artifact, or durable Tiger evidence was
removed. The trash directory is recoverable; it should be emptied only after
the current release and its evidence are independently archived.
