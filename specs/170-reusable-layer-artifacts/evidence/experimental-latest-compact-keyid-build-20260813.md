# Experimental latest baseline: compact-key-id AD build checkpoint

**Date**: 2026-08-13  
**Base**: `5e9e7aa8d921532a64451e48f112773a61a8f3c1` (`origin/Experimental`)  
**Fix commit**: `91056314f21077b0e5c8424637840f4b8421c5c7`

The public `Experimental` branch was ahead of the previous local checkout. It
already contained the Spec170 implementation merge and later source/test merges,
but it did not contain the compact-key-id associated-data fix. The fix was
applied only to `ServiceProvider::collaborationAssociatedData()`:

```text
wireKeyId = hybridCompactKeyId(keyId)
```

and the canonical wire identifier is now authenticated in the associated data.
No other source file was changed by this fix.

## Verification

The Spec170 Python/packaging suite on the remote Experimental source tree passed:

```text
65 passed, 2 skipped, 1 warning in 3.30 s
```

The first attempt had one sparse-checkout dependency omission
(`Experiments/NDNSF_NewAPI_Minindn_Perf.py`); after restoring that tracked
dependency, the suite passed. This was a checkout issue, not a source failure.

The core C++ library was configured and built with Boost 1.71 and the
experimental ndn-svs prefix:

```bash
PKG_CONFIG_PATH=/tmp/spec170-pc ./waf configure \
  --prefix=/tmp/ndnsf-experimental-latest-install
PKG_CONFIG_PATH=/tmp/spec170-pc ./waf build -j2 \
  --targets=ndn-service-framework
```

Result: **19/19, build finished successfully in 2m19.582s**.

The resulting local diagnostic library hash was:

```text
211fb5b7098db552adfbe032ce86f96b183fa3509b5279f899c1b037828ac04d
```

The initial build against the system ndn-svs headers failed because the
Experimental source calls the newer `getMappingFetchStats()`,
`getPublicationFetchStats()`, and `getPiggybackStats()` APIs. Reconfiguring with
the project’s experimental ndn-svs prefix removed that toolchain mismatch.

## Tiger boundary

This is a source/build checkpoint only. It is not a SIF, Qwen, CUDA, or Tiger
gate result. Spec170 T024--T029 remain open, and no D0/D1/D2a/D2b/D2h job was
submitted from this checkpoint.

The existing GitHub GPU workflow remains labelled and parameterized as Spec110.
Its latest inspected failure stopped at runtime closure generation with:

```text
RUNTIME_LIBRARY_MISSING:/opt/venv/lib/python3.10/site-packages/pillow.libs/libwebpmux-f0bc54e2.so.3.1.1
```

That workflow artifact cannot be promoted to an exact Spec170 candidate until
the release path is corrected or replaced with a Spec170-labelled immutable
OCI/SIF workflow.

