# B187 complete local candidate

## Scope and batches

用户要求补齐容器构建依赖，构建完整 NDNSF+APP 候选并在本机测试。复用 base `7b4b5010…`；生产源码固定 `587abb29` detached worktree，11 个无关生产改动不纳入。B187-NATIVE-INPUTS 负责封存官方 ONNX/Rust/Cargo；B187-BUILD-ONLY 只允许生成不可发布的 `BUILT_UNQUALIFIED` 候选；之后容器内构建与本地 YOLO 检查分别记录，不代替 MiniNDN/Tiger。

## Native inputs

五 lane：helper 源码封存、handoff 调用、容器原生构建接线、直接负例/模板测试、lock 与证据。r1 review 要求绑定 native manifest、更新调用文档和补直接测试；r2 `.codex-tmp/spec187-app-build-20260915/review-native-r2/` 返回 `STATIC_PASS / B187-NATIVE-INPUTS_COMPOSITION_PASS`。helper 保留失败目录和明确 FAIL 记录；当前 host 的 ONNX `-j4` 遵守 AGENTS 资源策略，不回写历史命令。

定向测试 `test_native_build_inputs.py`、`test_development_runtime_template.py`、`test_development_handoff.py`：23 passed，5.29s；日志 `native-tests-r1.log`。

首次实际 prepare 在 Cargo vendor 的离线输入边界失败：本机历史缓存缺少 `wasi v0.11.1+wasi-snapshot-preview1`（所有平台 vendoring 需要，旧 Linux-only build 未下载）。原始 `native-inputs/vendor.log`、`prepare-record.json` 为 FAIL，未启动 C++ 编译或 SIF 构建。下一步仅在准备阶段按现有 Cargo.lock 联网 fetch 缺项，再在新输出目录执行完全离线 vendor；不改 Cargo.lock、不允许容器构建下载 crate。

## Storage

准备结果更新：按原 Cargo.lock fetch 补齐 wasi 与 zerocopy-derive 后，`native-inputs-r2` 的 offline vendor 和封存成功；完整六文件摘要已写入 `development-handoff.lock.json`。首次 handoff 由于复用了依赖工作区中的未跟踪 `examples/example-trust-anchor.cert` 被拒绝（`handoff-r1.log`），未创建 bundle。改用三份依赖各自锁定提交的干净 detached worktree，避免将本机生成的身份资料纳入封存，不放宽 tracked-source 检查。

旧 `build-spec185-b3-asan-ubsan-fast` 已归档为 `.codex-tmp/build-retention-20260915/spec185-asan-build.tar.zst`，`tar --diff` exit 0 后释放原目录。二进制和原始记录仍可从归档恢复；没有删除模型、密钥、当前 normal build 或 Git 历史。另七个旧 build/static snapshot 的归档同目录，完成内容比较后再释放原目录。

## Current result

B187-BUILD-ONLY r2 五文件 `files.sha256` 核对一致，组合 diff SHA-256 `04c9e6156c5a6c1001b43d09fdee704887d9a51c325dcca6fc044deeb04ee85d`；官方 review-agent 返回 `STATIC_PASS / B187-BUILD-ONLY_COMPOSITION_PASS`，无发现。两个 CLI 反例测试通过（1.14s），`bash -n` 与 `git diff --check` 通过。静态漏检回顾：r1 发现旧验证器存在性检查未随 build-only 分支隔离，r2 已修复；compile-link/runtime-test 尚未观测。

实际完整构建已启动，Apptainer 1.5.3，日志 `.codex-tmp/spec187-app-build-20260915/build-r1/build.log`；候选输出 `Experiments/TigerCluster/images/spec187-complete-20260915/candidate.sif`。构建中不计 PASS。

`handoff-r2.log`：干净依赖 worktree 封存成功，`SOURCE_READY`，source seal `88ea6a1e49c198d5fce24327c1e7cbdf1225b11447ecdd724dfc60ff009bb2bf`。旧七目录归档 `old-native-build-snapshots.tar.zst` 的 `tar --diff` exit 0 后已释放原目录，当前可用约 18 GiB。

PARTIAL；完整 candidate 尚未构建，本记录不声明 LOCAL_PASS。
