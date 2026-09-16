# B187 complete local candidate

## Scope and batches

用户随后纠正分层：非 NDNSF＋APP 依赖须预置并验证于 base，不能每次 APP 构建重做 ONNX/Rust 等通用依赖。已暂停候选重试；`bundle-r3` 只是封存准备，未运行第二次构建。base 的依赖版本/ABI 变化需要重建并重新绑定摘要；NDNSF/APP 自身源码与 Cargo.lock 属于应用输入，不能因 base 可复用而省略候选验证。先前 base smoke PASS 保持历史事实，但不足以证明完整 SDK 闭包。

用户要求补齐容器构建依赖，构建完整 NDNSF+APP 候选并在本机测试。复用 base `7b4b5010…`；生产源码固定 `587abb29` detached worktree，11 个无关生产改动不纳入。B187-NATIVE-INPUTS 负责封存官方 ONNX/Rust/Cargo；B187-BUILD-ONLY 只允许生成不可发布的 `BUILT_UNQUALIFIED` 候选；之后容器内构建与本地 YOLO 检查分别记录，不代替 MiniNDN/Tiger。

## Native inputs

五 lane：helper 源码封存、handoff 调用、容器原生构建接线、直接负例/模板测试、lock 与证据。r1 review 要求绑定 native manifest、更新调用文档和补直接测试；r2 `.codex-tmp/spec187-app-build-20260915/review-native-r2/` 返回 `STATIC_PASS / B187-NATIVE-INPUTS_COMPOSITION_PASS`。helper 保留失败目录和明确 FAIL 记录；当前 host 的 ONNX `-j4` 遵守 AGENTS 资源策略，不回写历史命令。

定向测试 `test_native_build_inputs.py`、`test_development_runtime_template.py`、`test_development_handoff.py`：23 passed，5.29s；日志 `native-tests-r1.log`。

首次实际 prepare 在 Cargo vendor 的离线输入边界失败：本机历史缓存缺少 `wasi v0.11.1+wasi-snapshot-preview1`（所有平台 vendoring 需要，旧 Linux-only build 未下载）。原始 `native-inputs/vendor.log`、`prepare-record.json` 为 FAIL，未启动 C++ 编译或 SIF 构建。下一步仅在准备阶段按现有 Cargo.lock 联网 fetch 缺项，再在新输出目录执行完全离线 vendor；不改 Cargo.lock、不允许容器构建下载 crate。

## Storage

2026-09-15 用户再次要求先清理垃圾及过旧 Codex 会话，构建保持暂停。两个 9 月 6 日旧 build 归档为 `.codex-tmp/build-retention-20260915/old-sep06-native-builds.tar.zst`（约 851 MiB），`tar --diff` exit 0 后删除原 build-merge/build-abi，保留源码。六份临时/旧 checkout 的三个 RELEASE 大包分别与根目录保留件 `cmp` exit 0 后删重复文件，没有删除 Git 历史或原始运行日志。

Codex 两份旧故障备份从约 1.46 GiB 无损压缩至约 550 MiB，解压流与原文件 `cmp` exit 0 后移除未压缩副本。用户未选择永久删除期限，因此采用可恢复的 30 天归档：82 个会话约 328 MiB 压缩至 186 MiB；`tar --diff` exit 0，当前 thread 与可见打开文件排除，删除未压缩副本前再次核对 inode/size/mtime。归档、清单及恢复说明在 `/home/tianxing/.codex/session-backups/cleanup-20260915/`。未改 live Codex SQLite/index，近期/活跃大会话、封存 base、模型和密钥均保留。当前磁盘可用约 23 GiB。

准备结果更新：按原 Cargo.lock fetch 补齐 wasi 与 zerocopy-derive 后，`native-inputs-r2` 的 offline vendor 和封存成功；完整六文件摘要已写入 `development-handoff.lock.json`。首次 handoff 由于复用了依赖工作区中的未跟踪 `examples/example-trust-anchor.cert` 被拒绝（`handoff-r1.log`），未创建 bundle。改用三份依赖各自锁定提交的干净 detached worktree，避免将本机生成的身份资料纳入封存，不放宽 tracked-source 检查。

旧 `build-spec185-b3-asan-ubsan-fast` 已归档为 `.codex-tmp/build-retention-20260915/spec185-asan-build.tar.zst`，`tar --diff` exit 0 后释放原目录。二进制和原始记录仍可从归档恢复；没有删除模型、密钥、当前 normal build 或 Git 历史。另七个旧 build/static snapshot 的归档同目录，完成内容比较后再释放原目录。

## Current result

2026-09-16 清理与重排：删除了可重建的旧构建归档、旧候选 SIF/rootfs 和未占用的旧 Codex 会话文件，保留当前 Spec187 证据、失败日志、稳定 base SIF 及活动会话；磁盘余量由约 2.2 GiB 恢复到约 43 GiB。r12 的大文件仅作诊断，已删除其 candidate/rootfs，未沿用就地 header 修补。

2026-09-16 完整两阶段 candidate SIF 构建完成：Apptainer 1.5.3，SIF SHA-256 `e6cef05a949c3b865b35424ddb486bee05ea8a0023dd9ba4f7f5eda556541657`，大小 4,139,118,592 bytes；Waf C++ `293/293`（`-j4`，16m4.970s）、NDNSF 与 Repo binding wheel、builder/final `verify-native.py`、Python import、`ldd` 和 4278 项 SDK manifest 检查通过。构建记录明确为 `BUILT_UNQUALIFIED`，不是本地或 Tiger 资格；打包临时 rootfs 已在记录后清理，磁盘恢复约 35 GiB。definition/source seal 与构建记录位于 `Experiments/TigerCluster/images/spec187-complete-20260916-clean/` 和 `.codex-tmp/spec187-clean-restart/`。

`unit-r1` 首次候选内 C++ smoke 在 ELF 加载和测试编译后，于 fixture 创建 `/home/tianxing/.ndn` 时因 `--containall --no-mount home` 返回 134；该失败原始目录保留。run-di-unit-smoke.py 增加每次独立、owner 校验且 `0700` 的 HOME，并经官方 review-agent 复审 `STATIC_PASS`。`unit-r2` 使用同一 SIF/source revision `a0740640` 在容器内编译并运行 `di-runtime`、`di-preparation`、native plan 和 YOLO merge 四组 C++ tests，`DI_CPP_UNIT_SMOKE_PASS`，rc=0，52.56s，binary SHA-256 `b724eb792b9812bb8c76d48fcee4de68c30a87619ec30a8a136a28a533af721a`；记录 `.codex-tmp/spec187-clean-restart/unit-r2/`。

`yolo-r1` 首次 native runner 同样在 ORT/NDNSF-DI ELF 加载后因候选隔离 HOME 返回 134，失败原始目录保留。run-yolo-cpu-smoke.py 采用相同 `0700` HOME 修复并经官方 review-agent `STATIC_PASS`。`yolo-r2 --native-runner` 在候选 SIF 内编译并运行 ORT+C++ tensor codec/runner 三次，50 行输出每次最大绝对误差 `0.000534058`，`YOLO_CPU_NATIVE_RUNNER_PASS`，rc=0，9.76s，binary SHA-256 `f2d5d1cd6eddef9abec17829c63e13e7efde6da541c90b5bef05ca8f467703fd`；记录 `.codex-tmp/spec187-clean-restart/yolo-r2/`。

本轮已证明候选内 native library/binding 闭包和 C++/YOLO model smoke；尚未证明 host-gate/APP pair mutation、candidate-bound native requester config/input、真实 through-MiniNDN 两次请求、MiniNDN 负例或 Tiger promotion。`unit-r2`/`yolo-r2` 均是局部容器验收，不能把 T001/T002/T003/T007 或 LOCAL_PASS/Tiger PASS 提前关闭。

2026-09-16 B187-AUTHORITY-CLOSURE：为受保护 native requester 补入 `DI_NativeArtifactAuthority` 的 source seal、Waf target、builder/final 安装与 ELF/ldd/manifest 验证，以及 APP/validator 和 fixture 接线。官方 review-agent 对冻结快照 `.codex-tmp/spec187-clean-restart/review-authority-r2-20260916/changes.diff` 返回 `STATIC_PASS`，SHA-256 `36f3b9d71074149166764798a1fef949323f57c51111fb9bf2ce883531de8d07`，五 lane 无 P0–P3；此前 fixture 文件计数缺口已修正为 17。定向离线测试 `pytest -q Experiments/TigerCluster/tests/test_development_runtime_template.py Experiments/TigerCluster/tests/test_sif_app.py tests/python/test_prepare_local_sif_source.py` 为 39 passed、1 skipped（4.16s）。这是候选构建前的静态/脚本闭包证据；新的 SIF、authority runtime、through-MiniNDN 和 Tiger 仍未观测，T001/T003/T007 保持 `PARTIAL`。

2026-09-16 B187-PREPACK-CLOSURE T007 静态门：冻结快照 `.codex-tmp/spec187-clean-restart/review-t007-r1/`，`changes.diff` SHA-256 `23d2c6079e68d918fefdd141e9d3adfa31c7e62781eb959f6c802d582c6d0c0a`，官方 review-agent 返回 `STATIC_PASS`，无 P0–P3。审查确认 DI `.h/.hpp/.hxx/.ipp/.tpp` 完整安装和 sealed replay 逐字节比较、builder/final wiring、候选内 pkg-config C++ consumer flags 及文档边界。定向模板检查 `14 passed`，header/NDNSD 过滤子集 `2 passed`，Python 编译、`build-local-sif.sh` 语法检查通过；ShellCheck 仅报告既有 SC2015/SC1007 风格提示。实际容器编译、SIF 封装、C++/YOLO/MiniNDN 仍未观测，T001/T003 保持 PARTIAL。

r12 的 header-only 恢复脚本不再使用；下一候选将使用 HEAD `a0740640` 的干净 NDNSF worktree、稳定 base SIF `8ebfc464…` 和新的 source seal，从完整两阶段 definition 重新走 pre-pack consumer gate。

`unit-r2` 发现真实 header closure 缺口：current 安装目录的 AsyncDataflowRuntime.hpp SHA `7c371a6e…` 与 sealed source `5519d155…` 不同，缺少 RedistributionSpec；builder 安装逻辑只复制 Core hpp，遗留 DI hpp 来自父镜像。r12 不能计可交付。新增 header-only 修复批次：正式 builder/final 替换 DI headers，native verifier 比较完整头文件集及字节，回归拒绝缺失/旧/额外头。保留同一原生库，不调整 include 优先级掩盖错误。待审快照 `review-headers-r1`，`unit-r2` 日志/FAIL record 保留，compile-link 漏检。

`unit-r1` compile-link 失败：手写 fixture flags 未携带 `libnac-abe.pc` 的 `NAC_ABE_CMAKE_BUILD`，两个测试源包含 production header 时找不到配置头。候选 SDK 文件存在且 pkg-config 给出正确参数，修复 test driver 使用容器 pkg-config；不修改 SIF/生产二进制。实际 FAIL record 与 log 保留，属于 compile-link 静态漏检，未计单元 PASS。

r12 最终 SIF 构建与默认隔离 native verifier 均 exit 0，SHA-256 `c786bed830ecc12af8354819118a01dcae11564dd12e3b7d74e4db4f7630e7f8`，路径 `Experiments/TigerCluster/images/spec187-complete-20260915-r12/candidate.sif`。实际 receipt `build-r12/record.json` 保持 BUILT_UNQUALIFIED / NOT_AUTHORIZED，生产来源 `587abb29`。开始 `unit-r1` 容器内 C++ 编译/单元验收，尚未宣称模型或 MiniNDN PASS。

r12 fakeroot 复制完成，`copy-di.diff` / `copy-app.diff` 均为空（diff exit 0），92 环境文件 cmp exit 0；三个私有目录 UID/GID 165531:165531、mode 750 与原件一致。实际 mksquashfs 输入确认是 r12 新 rootfs 后，释放 r8 重复 rootfs 主体以避免双份目录＋压缩文件＋最终 SIF 的磁盘峰值；少量不可读私有空目录残留不计完整 rootfs。旧 recipe 已保存在 `app-runtime-r10.def`；Apptainer 将新复制树内 `.singularity.d/Singularity` 改为 localimage packing recipe，属于恢复封装 provenance，非生产代码变化。原 r8 不再可恢复，r12 在压缩中，尚无最终 PASS。

B187-FINAL-PACK-USER STATIC_PASS / COMPOSITION_PASS，diff `5f2131665db7a0b366384547b55f1fd2a6f0a185450654ac1962f671e75a8a07`。4 个 build-only/resume CLI 回归通过（4.70s），`pack-user-tests-r1.log`。已确认 r11 进程退出并仅删除其不完整复制目录，原 final rootfs 不动；r12 使用 fakeroot 重新封装，验收尚待结果。

r11 SDK/native 通过后，sandbox 复制暴露 `/run/ndnsf-di`、`/run/nfd`、`/tmp/ndnsf-di` 的容器 UID 私有目录不可由普通宿主用户读取。已中止（SIGINT）该打包，保留原 rootfs；恢复命令增补 `--fakeroot` 保持权限和 UID 映射，不能接受遗漏目录的镜像或放宽私有目录权限。首边界与日志保存于 `build-r11/build.log`，runtime-test 漏检。

r11 实际 BASE_DEPENDENCY_SDK PASS（4278 artifacts）与隔离默认 native verifier exit 0，随后进入 Apptainer final packing。日志 `build-r11/build.log`；这只证明待封装 rootfs 的 SDK/ABI/加载闭包，最终 SIF 自身与 C++/YOLO 仍待验收。

SDK-PROBE r1 与 ISOLATION r1 均经同一只读 reviewer 返回 STATIC_PASS / COMPOSITION_PASS；SDK diff `a636baba8da35581dfb9db2295197b0a03b1f57d1c1b5b19bb87b21b67a84029`。定向测试再跑 17 passed（4.86s），`environment-tests-r2.log`；r11 正在执行实际 SDK/native 复验与最终封装。两个 runner 禁用默认宿主绑定，仅保留声明的测试输入/输出；其实际结果仍待执行。

r10 首边界为 SDK-only probe 的库来源：修复后的默认 runtime 优先 current，SDK verifier 检测到 `libopenabe.so` 来自项目层而非 base。`build-r10/build.log` 保留，未启动封装；修正仅限 SDK probe 子进程的 LD_LIBRARY_PATH，native 默认环境检查不改变。属于 runtime-test 漏检，T001/T003 仍 PARTIAL。

B187-RUNTIME-ENV r4 `STATIC_PASS / COMPOSITION_PASS`，完整 diff SHA-256 `4ed43728c0ba15afecc1c2f741a8d9b24d3b5bd783f21211a55f343db9049dfe`；17 个定向测试通过（4.82s），`bash -n` 通过。`environment-tests-r1.log` 保存输出。已按受审 helper 为 retained final rootfs 增加同一 92 环境文件，原/有效 definition 与环境摘要见 `environment-delta-r10.json`；生产库不变。r10 从此 final rootfs 恢复封装，原 r8/r9 日志保持不变，未宣称正式资格。

2026-09-16 B187-RUNTIME-ENV：r1 静态发现临时恢复 helper 使用可被优化禁用的 assert 和跟随 symlink 写入，已改为显式检查、exclusive create 和原子 recipe 替换。r2 diff `2824240b879e794bd5f5d21deb75a63a368efc79182fc2e2583cec295d7e6dea` 又发现最终只读 SIF verifier 会写 manifest，需 disposable writable tmpfs；未先运行失败命令。r3 增加 `--writable-tmpfs` 与 CLI 回归，禁用 home/cwd/hostfs/bind-paths，等待静态/组合复审。两项均归类 static 发现；生产二进制保持 r8 构建身份，最终运行未验收。Context Mode project health PASS / active tasks hash stale，本轮进度以仓库原件为准。

r9 在打包前实际 clean container 复验中发现环境覆盖：SDK PASS，native import 因继承的 `91-sdk-environment.sh` 最后将 LD_LIBRARY_PATH 改回 base-only 而失败（rc=1）。这是新的运行时初始化缺口，不是 ABI 或缺失库；Core/DI 文件存在，final post 显式 export 曾掩盖此问题。正式模板增加 `92-ndnsf-environment.sh`，测试重放 base 91→repository 92 顺序。当前只实施环境增量，不重编库；保留 r8/r9 definition、原始日志及增量身份，重新做默认环境 native verifier 后再封装。

B187-FINAL-PACK 静态/组合通过，diff `cc3571672b2512e239f484ca0b23d681f9a1ec285662150bfe81978af62e4508`；P3 文档位置建议已改为独立 `Final SIF Packing Recovery` 标题。16 个定向 CLI/模板检查通过（4.68s），`bash -n` 通过，日志 `pack-tests-r1.log`。r8 builder stage 已归档并 `tar --diff` 通过后释放大部分临时目录，约 16 GiB 可用；r9 从完整 final rootfs 恢复封装，日志 `build-r9/build.log`，实际最终 SIF 仍待结果。

标准 r8：C++ 293 steps PASS（16m27.577s）、两组 binding、builder/final import/ABI/loader 均通过。最终 mksquashfs 完成后的 SIF copy 因磁盘不足失败（rc=255），未生成有效最终候选；首边界为 packing capacity。此前估算漏计临时 squashfs 与 SIF 同时占用，不宣称运行 PASS。完整 final rootfs `images/spec187-complete-20260915-r8/build-temp-956318929/rootfs` 保留。增加 build-only 的 final-rootfs 封装恢复入口，仍要求原两阶段 definition 门禁、definition/source-seal 字节一致、SDK/native verifier 和最终 SIF labels/hash；禁止用于 release PASS。

标准 r8 正在执行：`bundle-r8/app-runtime-r8.def` 通过正常两阶段入口，日志 `build-r8/build.log`；封存 base 不变，未使用恢复 definition。此前 stage 产物与恢复脚本归档至 `build-r7/verified-builder-stage.tar.zst`，`tar --diff` exit 0 后释放多数旧 rootfs，少量只读目录残留；旧 rootfs 已不完整，不能再恢复执行。原始构建/失败日志保留。容器单元 runner 与文档 r2 组合复审通过，实际运行仍待最终候选。

checkpoint `040c2dfd` 固化 stage-only 配置及门禁。r7 Repo wheel 与 builder 后续 import/native ABI/loader 检查全部 exit 0；但 `build-r7/final-build.log` 记录正常入口拒绝恢复 definition 的宿主 `%files` 二进制输入（rc=4，未构建 final）。准备层审查遗漏了最终入口 compatibility，归 runtime-test；不绕过门禁。为证明根本修复可从正式入口重现，改用已修复完整两阶段模板重新构建 NDNSF 层，复用封存 base。既有原生/绑定成果作为阶段验证保留，不计最终候选 PASS。

Repo library r2：只读 `STATIC_PASS / B187-REPO-LIBRARY_COMPOSITION_PASS`，diff SHA-256 `99090a51a223f2e311cf83475d42b814c202429e06302d9d71f3b114fb7470fa`；模板/handoff 定向检查 25 passed（4.30s），`library-tests-r2.log`。恢复脚本也通过同次审查和 `bash -n`；已从 Repo pip 边界启动 r7，日志 `build-r7/builder.log`，未重编 Core/DI/主 binding。

Repo library r1 静态/组合通过后，定向测试 22 passed / 2 failed：静态 boundary validator 仍硬编码旧 stage+base 字符串，拒绝新模板。失败日志 `library-tests-r1.log`。r2 同步门禁：显式 NDNSF 库目录仅 stage，依赖路径通过 pkg-config/prefix；新增对旧配置的拒绝反例，不放宽运行时 ABI/来源校验。

r6 builder：C++ 293 steps 编译链接全部通过（12m56.967s），主 ndnsf wheel 构建安装通过。随后 Repo binding 在 metadata 阶段因 `NDNSF_LIBRARY_DIR` 含纯依赖目录而拒绝（rc=1），未完成候选。首边界是 template/setup.py 调用契约，静态漏检归 runtime-test（构建脚本执行）；不归模型/协议错误。修正显式 Core 库目录为 stage-only，base 外部依赖仍来自 pkg-config/pinned prefix。复审后从 Repo binding 继续，原 C++ 与主绑定成果保留。

B187-CONTAINER-UNIT r1：官方只读 `STATIC_PASS / COMPOSITION_PASS`（bounded），冻结脚本 SHA-256 `0cbf002fb98dd71c79850a6708f71e77e3025e34cd0bbffd157b31f8481bd0f9`。同提交的 Runtime、Preparation、NativePlan、YoloMerge C++ tests 将在候选容器内编译，链接候选生产库；原生 fixture/oracle 仍为 C++。CLI help 与快照字节比较通过；实际编译/运行等待候选完成，状态 PARTIAL，不计 MiniNDN/Tiger。

恢复准备复审 `STATIC_PASS / B187-RECOVERY_COMPOSITION_PASS`；原 r3 保留为失败。`resume-builder.py` 比较两个 source archive 的所有旧文件字节/权限，唯一新增为 DI `.pc.in`，随后更新 seal、重放原 source/native 身份校验。当前在 retained rootfs 继续原 C++ `-j4` 构建（293 steps），日志 `build-r6/builder.log`。最终 stage 将沿用原 final post 和 validator；恢复准备通过不计候选 PASS。

SOURCE-CLOSURE checkpoint `016daa38`。恢复 recipe 的首次只读检查发生在 r6 render 完成前，因缺少 rendered definition 返回 STATIC_FAIL；未运行恢复。render 现已完成，definition SHA-256 `5d7fa93eee64e4b309b4776f7585fdbae0825f836b36aed528913a4e83c2d9a8`，seal `1fae2202ae463f4b0ed24c293aba6524c14aa26fc440356fc2e224c18fedca72`，已冻结全部输入等待复审。此为审查输入完整性问题，不是第二次构建失败。

B187-SOURCE-CLOSURE：只读 `STATIC_PASS / COMPOSITION_PASS`，diff `92128adf098066009a9dafcb1e2f952f25cb2222c9e0dd57d946b075e9088a20`；新增 Core/DI pkg-config 输入回归，handoff 13 tests passed（5.02s），`source-tests-r1.log`。新 bundle-r6 与 definition 已生成。用户建议增加容器内 C++ 单元验证：后续在候选内编译运行定向 DI tests，再运行真实 YOLO；不使用宿主测试二进制，不把单元 PASS 当完整协议资格。

build-r3 rc=255：SDK verifier、configure 与 Rust tokenizer staticlib 编译成功，Waf 在 task graph posting 时找不到 DI `.pc.in` 模板，未开始 C++ 编译。首边界为 source archive closure，静态漏检归类 compile-link。原始 `build-r3/build.log` 和 Apptainer rootfs 保留，拟补齐 sealer 清单后复用现场；base 身份不变，不计候选或协议 PASS。

build-r3 使用 `bundle-r5/app-runtime-r5.def` 已通过临时目录清理、SDK 复验和外部库 configure，正在编译仓库 Rust tokenizer bridge。脚本 checkpoint `d14b3f48`；实际输出仍待候选验收。额外磁盘诊断确认三个未占用 `.git/objects/pack/tmp_pack_*` 被 `git count-objects -v` 判为 garbage（7540688 KiB）；仅移除这三个失败临时文件后 garbage=0，未改正式 pack、loose objects 或历史。后续本地 checkpoint 命令禁用当次自动 gc，避免构建期间再次生成失败临时包。

scratch r2 已通过只读静态/组合审查；25 个模板/handoff/build-only 定向测试通过（3.75s），`bash -n` 通过，快照与工作文件一致。日志 `scratch-tests-r1.log`。开始生成 `bundle-r5` / 新 definition，准备重试完整候选；未宣称容器编译已通过。

恢复执行：builder scratch 统一移至 `/opt/ndnsf-build-work`，SDK 验证前设置 TMPDIR，pip/pkg-config/tokenizer 共用容器自有目录；不再删除固定宿主 `/tmp` 路径。r1 只读 `STATIC_PASS / B187-CONSUMER-SCRATCH_COMPOSITION_PASS`，diff SHA-256 `34c36810901add7a9cb4ea4c4316c2f6e7160477259c5724a07fb528971731e8`。r2 另加 Apptainer `--no-cleanup` 保留失败 rootfs，待增量审查与定向测试。build-r2 没有生成成功 build receipt；新增 `build-r2/failure.json` 是诊断记录，不能作为候选凭据。

build-r2 首边界为 builder 清理历史 `/tmp/nac-abe-build`、`/tmp/ndnsf-build-python`、`/tmp/ndnsf-pc` 的 Permission denied / Operation not permitted；base SDK 复验已 PASS，尚未启动 NDNSF 编译。记录 `.codex-tmp/spec187-app-build-20260915/build-r2/build.log` / `record.json` 保留。用户要求先清理磁盘尤其旧 Codex 会话，暂停重试；后续先修复临时目录隔离，不把该失败判为 SDK/协议失败。

续行 B187-NDNSF-CONSUMER：`bundle-r4` 为 SOURCE_READY，source seal `2815292f44c8f571ebbb73d375fc84765f3b971db07e08f43e58b24a91e76cdf`，新增已审查的 UAV probe 源文件并绑定封存 base。首个 render 调用因相对 bundle 路径被 `HANDOFF_BUNDLE_PATH_NOT_ABSOLUTE` 拒绝，未启动构建；原始 `render-r4.log` 保留，下一调用使用绝对路径。Context Mode project health PASS / active hash stale，进度以本地 Spec 为准。

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
