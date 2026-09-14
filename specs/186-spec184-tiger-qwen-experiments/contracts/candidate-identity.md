# Spec186 Candidate Identity Contract

## Purpose

本契约定义任何 MiniNDN、SIF 或 TigerCluster 结果可以引用的唯一候选身份。单一
commit、单一 SIF 或单次测试都不是候选。

## Immutable Tuple

每个 candidate manifest MUST 固定并 hash 以下字段：

```json
{
  "source": {"commit": "<40-hex>", "sourceSealSha256": "<64-hex>"},
  "runtime": {"baseSifSha256": "<64-hex>", "builderDigest": "<64-hex>", "abiManifestSha256": "<64-hex>"},
  "application": {"bundleSha256": "<64-hex>", "entrypointManifestSha256": "<64-hex>"},
  "harness": {"launcherSha256": "<64-hex>", "collectorSha256": "<64-hex>"},
  "configuration": {"profileSha256": "<64-hex>", "transportLayoutSha256": "<64-hex>"},
  "external": {"modelSha256": "<64-hex>", "tokenizerSha256": "<64-hex>", "inputSha256": "<64-hex>", "oracleSha256": "<64-hex>"},
  "validation": {"contractSha256": "<64-hex>", "schemaVersion": "spec186-candidate-v1"}
}
```

The manifest records the candidate paths and content hashes. Gate evidence MUST
record file sizes, modes, producer, creation command and relative evidence
references when those observations are available. An application bundle may be
an immutable directory;
its digest is the ordered hash of every relative file path and file content, excluding
the self-referential `bundle-manifest.json`. Private keys, model bytes and SIF bytes stay
outside Git; only their hashes and declared storage locations are recorded.

## Invalidation

| Change | Earliest invalidated gate | Required action |
| --- | --- | --- |
| source/header/native ABI | convergence/build | re-audit, rebuild affected consumers and re-run downstream gates |
| dependency/toolchain/SIF | build/runtime closure | create a new base identity and matching application bundle |
| application/harness/collector | convergence or local gate | rebuild/repackage affected plane and rerun all dependent executions |
| profile/identity/route/Slurm/Apptainer | profile/allocation preflight | regenerate effective config and rerun real-path evidence |
| model/tokenizer/input/oracle | model/local gate | create new workload identity and rerun numeric/behavior gates |
| evidence parser only | collector gate | run mutation suite; never rewrite old verdicts |

## Pre-Dispatch Closure

The gate MUST execute with no SSH, rsync, staging, scheduler or campaign call. It MUST:

1. resolve every path beneath its declared root and reject symlink escapes;
2. verify all declared hashes, sizes, modes and schema versions;
3. verify `_ndnsf.so` import, native entrypoint `--help`, `readelf -d` and `ldd -r` closure;
4. verify model/backend compatibility and the independent oracle identity;
5. reject stale evidence, mixed candidate digests, duplicate active run IDs and unknown fields;
6. emit a deterministic restart gate and zero-side-effect rejection receipt.

## Terminal Acceptance

`PASS` requires the same candidate digest in protocol events, numerical result, role/backend
records, process exit records and cleanup receipt. A transport, CUDA probe, ACK, READY,
fixture or partial native failure record cannot be promoted to a terminal PASS.

The Spec186 terminal receipt therefore includes `runId`, `protocol`, `numerical`, `roles`,
`process` and `cleanup` objects. Each object repeats the candidate digest; `protocol.completed`,
`protocol.terminalResponse`, `numerical.matched` with `independent=true` and a 64-hex
`oracleDigest`, every uniquely named role's `observed`/`backend`/`gpu`,
`process.allExited` with exit code zero, and `cleanup.reaped` must all be present. A marker
containing only `status`, `exitCode` and `cleanup` is rejected as incomplete evidence.
Role evidence must cover at least two uniquely named model roles from the declared YOLO
graph or an ordered `Stage<n>` Qwen pipeline, and a run ID must match the bounded run ID
syntax used by the scheduler.
