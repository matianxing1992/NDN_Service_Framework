# Remote Spec180 Candidate Reuse Audit

**Date**: 2026-09-07 (America/Chicago)
**Status**: INPUT-ONLY / NOT A SPEC183 QUALIFIED CANDIDATE

This note records a read-only inspection of the existing TigerCluster candidate
at `/project/tma1/ndnsf-di/candidates/spec180-runtime-b6710fd6`. No remote file
was changed and no job was submitted.

## What can be reused as an input

The candidate contains a physically complete-looking YOLO26n package and the
hashes match the historical Spec180 export record for the graph, weights, and
oracle:

| Item | Observed SHA-256 |
| --- | --- |
| `canonical/yolo26n.onnx` | `956ee2aa62f34c1ac035b85a837b70786bfa8da3ae8650e7539abbe572d0dd2a` |
| `canonical/yolo26n.weights` | `1a998d3d56c0103e57ea6df557370a219a3df53380572b4e9337ff26b4a94a7f` |
| `oracle/full-model-output.npy` | `ed5a23d73e3cda04677bfc9d895732b44e1455f20d9dc433ae5462eb7b9b7175` |
| `canonical-package/manifest.json` | `9c92d7526f19903a7cfd0acc0e764a466dd4b6d648fbab3a5f0eb640b07edf6d` |

The package catalogue includes `shared-backbone-two-shard-v1`, which is the
model graph required by Spec183. This makes the package a possible external
model-input source, subject to independent signature, registry, and provenance
verification. The package must not be copied into a run directory or treated as
qualified merely because these hashes match.

## Why the candidate itself is not reusable as Spec183 runtime

The observed run record is `spec180-yb-tiger-r1`, profile
`spec180-fixed-v1`, workload schema `spec180-dispatch-workload-v1`, and
candidate `spec180-yolo-r119`. Its profile and case configuration still use
the legacy `/example/group` Sync group and the Spec180 entrypoint. Spec183
requires the application instance namespace followed by `/sync`:

```text
group = applicationName + "/sync"
```

The candidate also has no Spec183 source seal, runtime/library manifest,
dispatch-plane receipt, or proof that its SIF contains the locked Spec183
dependency revisions. Its SIF hash is recorded only as historical evidence:

```text
sha256:b6710fd696a7f962f67f67a54278d92a15babb856c4af428a3f04ba54dc83285
```

Therefore it cannot satisfy T007, T008, T011, or any later TigerCluster gate.
The old SIF/profile/run record must not be used as a shortcut or mixed with a
new Spec183 source/runtime identity.

## Required next action

Treat the model package as `WAITING_EXTERNAL_INPUT` until its signed manifest,
trust-root registry, package provenance, and Spec183 case binding are verified.
Build a new local Spec183 SIF from the locked source and toolchain, then create
a new profile whose effective application namespace emits exactly
`applicationName + "/sync"`. Only that new candidate can proceed through the
T007 audit and the unit → integration → MiniNDN → local-SIF → Tiger sequence.
