# Layered Runtime Compatibility — 2026-09-08

## Current Observations

在完成209983真实GPU参考之后，继续正式NDNSF-DI外置应用迁移。
Raw: `Experiments/TigerCluster/results/yolo-layered-20260908/preflight/`。

| 检查 | 实际结果 | 动作 |
| --- | --- | --- |
| 当前外置DI Python包 + 历史SIF | APP_IMPORT_PASS，Python3.10.18、ORT1.20.0 | 复用明确的/app只读部署方式 |
| 当前User入口 | 保留/app/repo/examples/python/...层级后help通过；浅路径失败保留 | 不改应用算法，只固定打包布局及所需helper |
| 已有host Provider对SIF执行ldd -r | 缺NAC Consumer::clearCache、Producer::refreshPublicParameters、Consumer::refreshDecryptionKey；要求ORT VERS_1.26.0而SIF是1.20 | 不注入host.so；更新受影响base库/消费者，app按SIF SDK重新编译 |
| 本机Apptainer | /opt/apptainer/1.5.3/bin/apptainer实际1.5.3已存在 | 复用，不重新安装；/usr/local/bin的1.3.4仅为先前本地诊断 |
| 本地原始缓存SIF | 本次独立sha256=b6710fd696a7f962f67f67a54278d92a15babb856c4af428a3f04ba54dc83285 | 当前读取匹配Tiger，旧6a3d读取失败保持历史；不再误报当前不匹配 |
| recovery-20260907完整SIF | 同大小3525861376、同b6710fd6摘要，独立inode/唯一link | 用已验证相同字节的硬链接去重缓存，释放约3.28GiB，不重下载 |

两个SIF摘要结果分别保存在old-cache-sif-sha256.txt和recovery-sif-sha256.txt。
去重仅替换recovery目录内重复缓存的目录项，原始SIF与两个plane的硬链接保留。
不改旧manifest、源码或运行记录；完整分层组合仍未资格化。

## Base build graph checkpoint

`wscript --runtime-libraries-only` now registers the Core library, headers and
pkg-config metadata without DI objects, UAV programs or optional app recursions.
The default application build is retained. Three focused checks passed in
`results/yolo-layered-20260908/base-build-graph.xml`; this is build-graph evidence,
not a new SIF build or native runtime qualification. T011.layer remains open.

The generic Python binding currently compiles `NativeGrantVerifier.cpp` directly;
its two source/header files therefore remain a base binding dependency even when
the rest of DI is external. Changes to that binding dependency invalidate the
base. Directory names alone do not determine the deployment boundary.

## Base source and recipe checkpoint

The maintained source sealer now accepts `--selection base-libraries-v1`;
the legacy selection remains the default. Seven checks pass, including actual
archive/validator round-trip and exclusion of applications and compiled outputs.
The new selection contains 117 files; pinned NAC/SVS/NDNSD archives remain separate.

`library-runtime.def.in`, `build-base-libraries.sh`, `render-library-runtime.py`
and `verify-base-runtime.py` implement a single-stage base/SDK candidate build.
The same stable image retains compiler/headers for local external-app compilation;
Tiger executes applications only. This avoids retaining two expanded images on
the 14 GiB-free host (the inherited SIF expands to approximately 6.5 GiB).
Shell/Python syntax checks pass. Runtime execution of this new recipe is pending.

The renderer rechecked the inherited SIF and rejected `BASE_INPUT_DIGEST` before
writing a definition. Ordinary and sudo reads subsequently returned
`a2600783605752df995ec002f9eab915f35167196f9fc2e1cf62e6de6bd64e68`.
This supersedes the earlier same-day matching reads as a current integrity
checkpoint. A mistakenly attempted build after render failure only reported
the missing definition; it did not unpack or compile. No current base has been
produced, and no formal NDNSF-DI run has been submitted from these files.

Follow-up: direct I/O matched the lock; file-specific page-cache eviction restored
ordinary reads and the renderer's independent digest check. The single-stage
build is now running with a 2700-second bound and at most two compile jobs.
See `input-read-integrity.md`; do not describe this as a permanent host repair.

Third build checkpoint: source copied through direct I/O into a verified tmpfs
SIF; extraction and APT succeeded. NAC, SVS (system Boost 1.71), NDNSD and Core
compiled/installed; Python bindings are now building. The base SIF is not yet
complete or qualified. See `base-build-3.log` and the live build, not stale tool
session handles after goal continuation.

The external app source seal at `.cache/layered-base-20260908/app-source` contains
456 files from revision `81e330eaee6bb6109735b7a13b570d63dc7b7771`. All 117 base
source records match the running base build exactly. The TF32 API compatibility
repair therefore changes only the external app. The old host1.26 syntax check
was insufficient: actual SIF1.20 headers lack the newer C++ options owner. The
V2 C API replacement passes the one affected policy check; full exact-SDK app
compilation and runtime qualification remain pending.
