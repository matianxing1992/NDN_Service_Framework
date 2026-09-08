# T004.i — allocated node storage ownership

Date: 2026-09-07. Status: IMPLEMENTED, component evidence only. T004 remains
incomplete and T007 remains BLOCK. No actual SIF execution, Slurm allocation,
GPU inference or full-size SIF copy was run for this change.

## Change and ownership

`runtime/yolo_storage.py` is included in the frozen 25-file harness. Normal GPU
owners enter it after Slurm capture. It measures filesystem blocks/inodes,
rejects network/unsupported filesystems and unsafe directory paths, and creates
private per-rank scratch. Actual fsync/socket probes precede a complete size/SHA
verified SIF copy. Native owners receive its physical SIF and NFD node paths.
The copy and issuer share one staging deadline. Repo receives observed capacity
instead of treating the profile's requested byte budget as available capacity.

Successful workload cleanup removes only owned scratch; failure retains it.
Durable run-root receipts record staging and final cleanup. GPU collection and
offline reanalysis reject missing/failed/mismatched records and mismatched
two-node SIF identities or duplicate host labels. This does not prove quota,
reserve space or replace T012's real environment qualification.

## Bounded validation

Results directory: `Experiments/TigerCluster/results/spec183-node-storage-20260907/`.

| Artifact | Result | Scope |
| --- | --- | --- |
| focused.xml | 39 passed, 3 failed | Initial storage/local/distributed/profile checks; three old GPU fixtures required the original dictionary object instead of the new staged SIF mapping |
| boundaries.xml | 56 passed, 7.90 s | Corrected local owner mapping, real provision deadline wrapper, frozen bundle, allocation entry boundaries |
| storage-cli.xml | 66 passed, 19.510 s | Final storage mutations, distributed owner composition and public CLI checks |

The final two suites are reported as executed groups, not an asserted unique-test
total. Storage tests use real tiny local files, distinct copied inodes, fsync,
Unix sockets and deletion. Native launch and Slurm in orchestration tests are
explicit doubles; two host labels in file tests do not constitute two real nodes.
Retain the initial failed artifact; no model rerun was used to fix its fixture.

## Remaining path

The actual dispatch refresh failed twice with `FILE_DIGEST:baseSif` before
rendering the new harness. Retained retry stderr: `render.err` in this result
directory. A separate 1 MiB streaming read observed
`6a3d001088305a9e189c7e97fe1ed19c8167347341de1ca23a1e67076f564b94`
instead of the pinned
`b6710fd696a7f962f67f67a54278d92a15babb856c4af428a3f04ba54dc83285`.
Size was 3525861376, mtime_ns 1788532683000000000, ctime_ns
1788828633121216579, unchanged during that 19.45-second read. This is an
unresolved content/read-integrity failure; stable metadata does not establish
correct bytes. The older failure-log's sandbox explanation is not proven for
this occurrence. No recorded hash was replaced and no further blind refresh
was run. Source harness requires 25 files, but the new actual frozen candidate
has NOT been generated or verified. The public dispatch check cannot qualify it.

Finish remote transport/portable retained prerequisites, the negative-dependency
owner and actual terminal-state reconciliation. Then re-audit T007 and follow
unit → integration → MiniNDN → exact local SIF → bounded Tiger gates. Existing
matching build evidence is reusable; storage harness changes alone do not justify
rebuilding native libraries or repeating historical tests.
