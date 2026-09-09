# Exact-SIF MiniNDN Y-B runtime evidence

**Run:** `minindn-local-20260908-v25-yb21`
**Date:** 2026-09-08
**Scope:** local exact-SIF, CPU-backed MiniNDN Y-B only

The run used the immutable base SIF
`base-runtime-controller-version-j4-v12.sif` (`sha256:42f45f20d152b5ef6efa0abad01f6b5b3fc5cdb7ef08ea22e6fae1ee2b10ea34`)
and the matching external application bundle
`app-controller-version-j4-v18` (source seal `fc88c2e15c9d5c7d9dd9d3a0c9c962c7b3f2502b`,
builder identity `8da0ade5...`). The preparation digest was computed from the
run's `public/preparation.json`; no stale v17 SIF digest or prior private
identity material was reused.

The exact MiniNDN driver returned `T010_DONE` with return code `0`. Runtime
evidence shows:

- Controller/User/Provider trust-schema startup and protected permission grant;
- four provider ACKs, committed selection, and execution of all four providers;
- terminal response `status=true`, result digest recorded in the lifecycle journal;
- numerical oracle match: `matched=true`, `maxAbsError=0.0005340576171875`,
  shape `[1,50,6]`;
- cleanup observation `clean=true`, with all child processes reaped and no
  remaining network resources.

This closes the **normal exact-SIF Y-B application/protocol/data-plane slice**
locally. The driver intentionally reports `qualification=NOT_EVALUATED`; this
is not the formal three-scenario T010 qualification, a TigerCluster run, or GPU
evidence. Y-A, Y-N, host qualification-manifest production, one-node GPU, and
two-node Tiger execution remain open.

The failures preceding v25 were launch-contract defects isolated by the same
workload: missing `/config` trust-root binding, regenerated post-provision
keychains, wrong role HOME, root/UID ownership of the envelope key, conflicting
recipient-map environment, and shell descendants that delayed namespace cleanup.
They did not reach YOLO inference until v24/v25.
