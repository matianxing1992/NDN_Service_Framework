# Spec170 foundation dependency correction — 2026-08-13

## Failed immutable-build checkpoint

- Workflow: `Experimental NDNSF-DI Spec170 foundation image`
- Run: `31686035970`
- Source: `Experimental@ce7ae82907e632db6796af8f423801c7e25ca50b`
- Result: `BLOCKED` during the foundation `./waf` build; no OCI image or
  digest was published.
- Failure location: `ServiceUser.cpp` compilation against the sealed
  `ndn-svs@7b616b08624a79617bb05f2d3553bbbacdc4c482` headers.

The locked dependency was an older publication-fetch revision. It did not
provide the V3/runtime surface used by the current NDNSF source, including
`SVSPubSubOptions::syncProtocol`, `SvsProtocolVersion`,
`getSyncProtocolOptions()`, and the fetch/piggyback statistics accessors.
The failure is therefore a dependency-identity mismatch, not a Python lock,
GPU, disk, or source-compilation failure.

## Corrected dependency candidate

The local ndn-svs `master@3c96ab4` history plus the pending runtime
diagnostics/publication-worker patch was built in an isolated worktree as:

```text
ndn-svs@060811333de68b9674e45522222a14d4e047bf28
```

The commit was pushed to the dedicated remote branch `spec170-runtime-v3`.
With `PKG_CONFIG_PATH` pointing at the local ndn-cxx build and `PATH` ordered
so `/usr/bin/ld` is selected, the candidate built successfully and its full
unit-test binary reported:

```text
Running 82 test cases ... No errors detected
```

NDNSF `gpu.lock` now records this exact commit. The lock-parity/spec170 Python
and container suite remains green: `69 passed, 2 skipped, 1 warning`.

The first corrected-lock dispatch (`31687612105`) stopped at the existing
preflight guard because that guard still hard-coded the old revision. The
guard is now updated in
`packaging/ndnsf-di-container/oci/scripts/preflight-gpu-build.py` to require
`060811333de68b9674e45522222a14d4e047bf28`; this is a source/preflight
correction, not a bypass. A new dispatch is required.

This evidence does not close T024: the foundation workflow must be rerun from
the corrected lock and corrected preflight, and must produce a signed immutable
manifest before any GPU image or exact-SIF gate is attempted.
