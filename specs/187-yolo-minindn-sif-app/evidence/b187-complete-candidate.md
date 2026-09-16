# B187 complete local candidate

## Scope and batches

用户随后纠正分层：非 NDNSF＋APP 依赖须预置并验证于 base，不能每次 APP 构建重做 ONNX/Rust 等通用依赖。已暂停候选重试；`bundle-r3` 只是封存准备，未运行第二次构建。base 的依赖版本/ABI 变化需要重建并重新绑定摘要；NDNSF/APP 自身源码与 Cargo.lock 属于应用输入，不能因 base 可复用而省略候选验证。先前 base smoke PASS 保持历史事实，但不足以证明完整 SDK 闭包。

用户要求补齐容器构建依赖，构建完整 NDNSF+APP 候选并在本机测试。复用 base `7b4b5010…`；生产源码固定 `587abb29` detached worktree，11 个无关生产改动不纳入。B187-NATIVE-INPUTS 负责封存官方 ONNX/Rust/Cargo；B187-BUILD-ONLY 只允许生成不可发布的 `BUILT_UNQUALIFIED` 候选；之后容器内构建与本地 YOLO 检查分别记录，不代替 MiniNDN/Tiger。

## Native inputs

五 lane：helper 源码封存、handoff 调用、容器原生构建接线、直接负例/模板测试、lock 与证据。r1 review 要求绑定 native manifest、更新调用文档和补直接测试；r2 `.codex-tmp/spec187-app-build-20260915/review-native-r2/` 返回 `STATIC_PASS / B187-NATIVE-INPUTS_COMPOSITION_PASS`。helper 保留失败目录和明确 FAIL 记录；当前 host 的 ONNX `-j4` 遵守 AGENTS 资源策略，不回写历史命令。

定向测试 `test_native_build_inputs.py`、`test_development_runtime_template.py`、`test_development_handoff.py`：23 passed，5.29s；日志 `native-tests-r1.log`。

首次实际 prepare 在 Cargo vendor 的离线输入边界失败：本机历史缓存缺少 `wasi v0.11.1+wasi-snapshot-preview1`（所有平台 vendoring 需要，旧 Linux-only build 未下载）。原始 `native-inputs/vendor.log`、`prepare-record.json` 为 FAIL，未启动 C++ 编译或 SIF 构建。下一步仅在准备阶段按现有 Cargo.lock 联网 fetch 缺项，再在新输出目录执行完全离线 vendor；不改 Cargo.lock、不允许容器构建下载 crate。

## Storage

准备结果更新：按原 Cargo.lock fetch 补齐 wasi 与 zerocopy-derive 后，`native-inputs-r2` 的 offline vendor 和封存成功；完整六文件摘要已写入 `development-handoff.lock.json`。首次 handoff 由于复用了依赖工作区中的未跟踪 `examples/example-trust-anchor.cert` 被拒绝（`handoff-r1.log`），未创建 bundle。改用三份依赖各自锁定提交的干净 detached worktree，避免将本机生成的身份资料纳入封存，不放宽 tracked-source 检查。

旧 `build-spec185-b3-asan-ubsan-fast` 已归档为 `.codex-tmp/build-retention-20260915/spec185-asan-build.tar.zst`，`tar --diff` exit 0 后释放原目录。二进制和原始记录仍可从归档恢复；没有删除模型、密钥、当前 normal build 或 Git 历史。另七个旧 build/static snapshot 的归档同目录，完成内容比较后再释放原目录。

## Current result

最新结果：最终 base SIF 真实验收并永久封存，SHA-256 `8ebfc4646a5f96109a8480b120e684ee3923bf067d2e49aef53b42d29e5acdd9`。基础/SDK probe、坏库覆盖拒绝、三次 C++/ORT YOLO CPU oracle 对照、封存清单校验与移动后复验均通过；见 [base 封存记录](b187-base-sdk-sealed.md)。仅 B187-BASE-SDK DONE，NDNSF consumer 实际候选仍待构建，T001/T003 保持 PARTIAL。下文保留先前阶段和失败边界。

consumer r2 复审通过；同批四组测试最终 46 passed（5.59s），`consumer-sdk-tests-r3.log`。SDK rootfs 基础 smoke 也通过。r2 使用 Apptainer 的 sandbox→SIF 路径继续封装 r1 已编好的依赖，只更新验收脚本与等价 runtime metadata，不重新编译外部库。封装复制完成且 SDK manifest/脚本字节比较一致后释放旧临时 rootfs，保留 r1 原始 FAIL、输入和日志；释放可重建的 VS Code C++ 缓存以保证磁盘空间。最终 SIF 验收及永久封存仍待完成。

SDK r5 单行 include 修复通过只读增量审查；保留 rootfs 的真实 `dependency-sdk.py verify` 已 PASS（C++/Rust/Python/GStreamer/ELF，4278 项 manifest 文件），日志 `sdk-rootfs-verify-r2.log`。这仍非最终 SIF 验收。NDNSF consumer r1 静态/组合审查无控制性缺陷；批测试 45 passed / 1 failed，失败为旧 handoff fixture 仍要求外部 NAC 源码出现在 NDNSF definition。已修正契约断言，待复审复测。

SDK r1 实际失败：ONNX/NAC-ABE/SVS/NDNSD 编译安装完成；C++ probe 在 `nac-abe/algo/public-params.hpp` 引用 `common.hpp` 时缺少 NAC include 子目录。首边界为 SDK probe compile，非生产推理。原始 `images/base-sdk-20260915-r1/build.log` / FAIL record 保留；`--no-cleanup` 保留 rootfs，计划仅修复 probe include 后原现场复验，避免重编依赖。静态漏检记为 compile-link，实际 SDK PASS 尚未获得。

最新 B187-BASE-SDK r4：只读 reviewer 核对完整快照后返回 `STATIC_PASS / B187-BASE-SDK_COMPOSITION_PASS`，无控制性缺陷。快照 `.codex-tmp/spec187-two-layer-20260915/review-base-sdk-r4/` 含新文件与 `files.sha256`。修复 r1 的实际头文件/符号调用、GStreamer 闭包、loader 来源限制与 dispatch/父镜像反例，以及 r3 的 ONNX checker/shape inference 验证缺口；额外实际检查发现旧 base 缺 pybind11，已将锁定五个 Python wheels 纳入 base。定向测试 24 passed（1.19s），日志 `sdk-tests-r1.log`。

已启动现有 base 的增建，输出 `Experiments/TigerCluster/images/base-sdk-20260915-r1/`；构建原始日志 `build.log`，入口日志 `.codex-tmp/spec187-two-layer-20260915/base-sdk-r1.log`。静态通过与脚本测试不等于 SDK 构建成功；compile-link/runtime-test 尚待实际结果，不声明 candidate、MiniNDN 或 Tiger PASS。

B187-BASE-SDK / PARTIAL：现有 `build-base-sif.py --dependency-bundle` 扩展同一入口；父镜像为已验证 `7b4b5010…`，不重新执行基础修复。外部库/headers 安装 `/opt/ndn-base`，ONNX/Rust/vendor 与 source/build pair 纳入 SDK 身份；实际 C++/Rust probe、ELF、包/文件变更负例负责 SDK 验收。五 lane 为构建入口/依赖实现、handoff/base 调用、template、反例与 C++ probe、契约/技能/证据。只读审查快照 `.codex-tmp/spec187-two-layer-20260915/review-base-sdk-r1/`，尚未运行 SDK build。

技能核对发现仓库 `skills/itiger-ndnsf-ops/SKILL.md` 为 62 行简版，而本机安装为 2932 行历史副本；本轮在简版加入用户确认的两层与增建规则后同步安装，两份字节一致，`skill-creator/scripts/quick_validate.py` 均通过。旧安装版备份保留，不把技能更新当构建验证。Context Mode project health 通过，active-Spec 索引 hash 过时；当前进度以工作区 Spec/源码为准。

build-r1 rc=255：ONNX、NAC-ABE、NDN-SVS、NDNSD 编译安装成功；NDNSD 的 include flag 输出检查误拒绝被环境路径过滤的结果。改用 metadata includedir 精确比较；增加真实 pkg-config 过滤反例。下一次构建将模板历史 `-j2` 统一为本机政策 `-j4`（Rust 工具内部限额不变），保存新 definition，不改 r1。

空间补充：四份旧安装前缀归档 `old-install-prefixes.tar.zst`，内容比较 exit 0 后释放原目录；三份 Spec181 checkout 的 RELEASE 压缩包经 SHA-256 与根目录保留文件比较一致后删除重复副本，仍可从 Git/根目录恢复。

B187-BUILD-ONLY r2 五文件 `files.sha256` 核对一致，组合 diff SHA-256 `04c9e6156c5a6c1001b43d09fdee704887d9a51c325dcca6fc044deeb04ee85d`；官方 review-agent 返回 `STATIC_PASS / B187-BUILD-ONLY_COMPOSITION_PASS`，无发现。两个 CLI 反例测试通过（1.14s），`bash -n` 与 `git diff --check` 通过。静态漏检回顾：r1 发现旧验证器存在性检查未随 build-only 分支隔离，r2 已修复；compile-link/runtime-test 尚未观测。

实际完整构建已启动，Apptainer 1.5.3，日志 `.codex-tmp/spec187-app-build-20260915/build-r1/build.log`；候选输出 `Experiments/TigerCluster/images/spec187-complete-20260915/candidate.sif`。构建中不计 PASS。

`handoff-r2.log`：干净依赖 worktree 封存成功，`SOURCE_READY`，source seal `88ea6a1e49c198d5fce24327c1e7cbdf1225b11447ecdd724dfc60ff009bb2bf`。旧七目录归档 `old-native-build-snapshots.tar.zst` 的 `tar --diff` exit 0 后已释放原目录，当前可用约 18 GiB。

PARTIAL；完整 candidate 尚未构建，本记录不声明 LOCAL_PASS。
