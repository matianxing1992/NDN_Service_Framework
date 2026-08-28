# Spec170 local storage cleanup — 2026-08-19

This cleanup was limited to one explicitly superseded, user-trash directory:

```text
/home/tianxing/.local/share/Trash/files/ndnsf-spec170-route-cleanup-20260817
```

The directory contained two rejected/pre-RPATH SIFs, one superseded source
archive, and the historical Docker/OCI/remote-materialization diagnostics
listed by the 2026-08-17 route-cleanup record. It was not referenced by the
current r23 candidate or its active bundle. No current SIF, base SIF, build
tree, results directory, source checkout, or tracked evidence was removed.

| Check | Result |
|---|---|
| Space before cleanup | 53 GiB available, 69% used |
| Trash directory size | 17,711,068 KiB (about 16.9 GiB) |
| Space after cleanup | 70 GiB available, 59% used |
| Removed path | exactly the directory above |
| Recovery | permanently removed from desktop Trash; not recoverable through Trash |
| Current r23 SIF | retained |
| r23 base SIF | retained because current definitions and build records still reference it |

The next update must still use the cheap identity gate and must not create a
second SIF until the candidate/source/lock identity and disk headroom pass.

After this named-trash cleanup, the Apptainer cache was checked and cleaned
separately with the qualified binary:

```text
/opt/apptainer/1.5.3/bin/apptainer cache clean --force
```

It removed approximately 4.5 GiB of disposable OCI cache data. The active r23
SIF and its base SIF were not in that cache and were retained. The subsequent
reuse-gate check reported 75 GiB available (56% used), and
`apptainer cache list` reported zero cached container/OCI blobs. This cache
cleanup does not change the release identity or qualify the dirty working
tree; it only provides disk headroom for a future, source-bound candidate.
