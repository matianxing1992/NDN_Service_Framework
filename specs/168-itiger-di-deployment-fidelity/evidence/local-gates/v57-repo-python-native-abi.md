# Spec 168 v57 Gate B - DistributedRepo Python/native ABI mismatch

## Verdict

`BLOCK` — `EXEC_LOCAL_GATE_PROCESS_FAILED` after `19252.378 ms`; no NDNSF
Request began and `automaticRetry=false`. Topology, routes and policy generation
completed, then the Controller failed its first application import.

## Frozen identity

- Candidate: `20260804T084343Z-v57-canonical-overlay-launch`
- Source identity: `sha256:3920f7a4964c50f3b742c6dd4dacf3b22f09484d2f66537388d817dbd93ec0f0`
- Gate command: `sha256:3545adcb6ec13f41d6892ed7996a75985bd439a4eb818d0e8e054461b25f8944`
- Gate manifest: `sha256:631e98e5a01747dab6c3f59b9a055e8d72bd56f25e8d563f143988d7d034d39b`
- Gate checkpoint: `sha256:d82999c29e994c2ce1ed7418078a007d657f131021569480214b0dbed5b745ba`
- Launcher log: `sha256:4cce3518d23da1f6c8691e1db2c1158a40b22a8c28adc4f5baa7a7edfeae9088`
- Controller log: `sha256:1e51e9c33debb821c03a3abbf978b5c89ce360b175118b507a29f308382eebb6`

## Narrowest boundary and cause

The workspace `py_repoclient` Python package imports
`AdaptiveArtifactTransfer`, but its CPython 3.10 extension
`sha256:8472ce013d9b2662f15ec482d01da4b02fdfaf4771634a06af1201c5391f3261`
does not export that symbol. MiniNDN rebuilt the node `PYTHONPATH` with the
workspace Repo package before the entrypoint runtime overlay, so Controller
import failed before readiness.

The immutable v49 image already contains a compatible extension:

```text
sha256:3121dc524921d8a4e2a094f5ca05baaa084283549ec9aa1face0b9b36f579af4
```

A disposable-container composition test proved that the current Repo Python
package, this image extension, the current NDNSF core/CPython ABI overlay, and
`APPController` import together. No Repo or NDNSF library rebuild is required.

## Repair boundary

The MiniNDN harness now treats a present `SPEC168_OVERLAY_ROOT` as the
authoritative runtime closure and preserves inherited overlay paths before any
workspace package. The overlay entrypoint additionally imports and identity-
checks `AdaptiveArtifactTransfer` before launching MiniNDN. The v57 exact-name
container cleanup completed: no container, NFD or NLSR remained.
