# GitHub sealed-source primary-route gate

**Date**: 2026-07-13  
**Task**: T158  
**Implementation revision**: `97c1aafb7375e694b460623d262f6a0496c26e27`  
**Verdict**: `PASS` for local/offline readiness only

This gate validates the repaired GitHub Buildx source path without claiming a
GitHub OCI build, GHCR digest, iTiger SIF, GPU runtime, or Qwen inference result.

## Public exact-source reachability

The two previously unreachable locked commits were published under dedicated
advertised refs:

- NFD `2b43d675e3fba37b5362fab0001ba542bb07b43b` at
  `matianxing1992/NFD:spec110-sealed-2b43d675e3fb`;
- ndn-svs `5b5461a728012e9d0959e99ef0acbc5f32fc9d25` at
  `matianxing1992/ndn-svs:spec110-sealed-5b5461a72801`.

Anonymous exact-SHA fetch and archive creation succeeded for all six entries in
`gpu.lock`. A second seal created from local repositories produced byte-identical
dependency archives:

| Dependency | Revision | Archive bytes | Archive SHA-256 |
|---|---|---:|---|
| NAC-ABE | `1cc17d9d21f4dfc0921cc77315d0c57d46291880` | 532480 | `eb2261bc97fcee39f032d4a2c8b2255ae7a521dd06120f2d6f99749a0ddde971` |
| NDNSD | `25f7ad9d2f8848c10025e71358e3dafd62c348c0` | 1290240 | `85776653fe3cf1880da77ce506244c0dbf939e03a09d9d3a78aed79f0172dd43` |
| NFD | `2b43d675e3fba37b5362fab0001ba542bb07b43b` | 3911680 | `7dfade7d7f8ba0cf282f4c28c88095f4d7c695a846ba5329d54811afdac640c7` |
| ndn-cxx | `8296fc9462c7ef9635b7c45468c067fd39514e31` | 4526080 | `9e76a7a0dec4f461546eaeff13fa4537beef3a2d67d118635041e1a0dd5629c0` |
| ndn-svs | `5b5461a728012e9d0959e99ef0acbc5f32fc9d25` | 460800 | `dacf6c8bdc0c83a33a266c261d44b940b5ca7ddf5158db5a4992e48de7a8c9cb` |
| openabe | `b8f9d3c8a2620c1185ca972248f7af39c1eae68c` | 2355200 | `b229d4f143275790a3afad291d27e1017f617c0ae8b0cabe628e6426293c3f9f` |

The local and remote seal record digests differ only because each immutable
record has its own `createdAt` value. Their workspace revision, workspace
archive digest, lock digest, dependency revisions, archive byte counts, and
archive digests match.

## Runner-disk and forbidden-content boundary

Git LFS initially hydrated three historical `RELEASE/` payloads while measuring
the workspace, inflating the archive from about 67 MB to about 906 MB. The seal
now disables LFS process/smudge filters and records raw Git-object content;
`.dockerignore` excludes `RELEASE/`, results, third-party trees, model weights,
ONNX exports, GGUF, checkpoints, SIF files, and key material. The final raw
workspace archive was:

```text
bytes=66703360
sha256=edb2d8468fbbf17ab29d855a47be84a7ec6fe2c10e1ae8359b89e47318061e42
```

Both source scans covered six archives, 1,696 files, and 12,035,506 bytes with
zero findings. The identical scan record digest was
`sha256:3af1d5b7a24a0044922d6019880608a4e628f8a7d3cb15d3fc3da3743a50bff0`.

## Verification

```text
Spec 110 offline Python suite: 86 tests, 0 failures, 0 errors, 0 skipped
JUnit SHA-256: 93f8199b1b429738215ad134b564050a91b8f4215c319a2c46dfb4120b2f4580
rootless-build integration: PASS
release/materialization integration: PASS
runtime compatibility: PASS (6 cases)
network scripts: PASS
packaged security contract: PASS
ShellCheck: PASS
workflow YAML parse: PASS
strict Spec Kit structural audit: PASS (37 FR, 160 tasks, 37 traced FR)
local/remote dependency archive comparison: IDENTICAL
```

Local Docker Buildx is version `v0.14.0` and does not expose `buildx build
--check`; `actionlint` is not installed. These unavailable optional static tools
are not represented as passes. The full Buildx execution remains T159.

## Execution boundary

```text
githubWorkflowRun=NOT_EXECUTED
ghcrDigest=NOT_AVAILABLE
slurmSubmission=NOT_EXECUTED
runtimeSif=NOT_AVAILABLE
qwenWeightsInImage=FORBIDDEN
nextTask=T159_EXACTLY_ONE_PUSH_TRIGGERED_RUN
```

