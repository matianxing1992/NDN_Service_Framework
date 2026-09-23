# Base SIF + NDNSF

2026-09-19 高层约束补充见 [S1–S3](../../../Design/highlevel-design.md#sif-构建与复用约束)：复用依赖 base，在对应容器环境内构建 NDNSF，最终交付新完整 runtime SIF。base 是构建输入，最终运行不依赖相邻 base 或外置应用包；旧 pair/APP 入口保留兼容身份，不代替这一交付要求。

2026-09-15 用户接受两层交付。本文是当前构建归属规则；旧三层提法和历史 APP 归属在冲突处由此替代。

| Layer | Contents | Invalidation |
| --- | --- | --- |
| base SIF | 已声明外部 runtime、工具链、headers、pkg-config、Python ABI、ONNX/ORT、Protobuf、Rust、NDN-CXX/NFD、NAC-ABE/SVS/NDNSD 等依赖 | 外部版本、选项、ABI 或依赖集合改变 |
| NDNSF | 本仓库 Core、DI、Repo、UAV、Python bindings、应用/实验代码与选定构建目标 | 对应源码、配置、harness 或 base identity 改变 |

这是代码归属，不声称每个模块已完成运行资格。当前首先验证 YOLO CPU；QWEN 保持 TODO。
旧 `/opt/ndnsf-app`、APP manifest 等名称可以作为兼容表示承载整个 NDNSF 层，不创建额外第三层。
旧冻结 APP 校验所需的外部库保留原布局；当前 `installed-v1` 完整 SIF 不再导出 APP，
不复制外部库到 NDNSF 安装树，直接消费 base SDK。禁止在 NDNSF builder 重新编译它们。
构建模板先验证 base SDK，再核对 source seal 的依赖摘要及 tokenizer Cargo 声明；不匹配即停止。
NDNSF 层的模型、身份、profile 和输入仍独立绑定；模型/私钥不因此嵌入 base。

## Incremental Base Completion

优先用已验证 base 作为父镜像，复用现有 `build-base-sif.py --dependency-bundle` 入口，
消费已验证 handoff 中的外部源码和锁定工具链。生成新 SIF 和新摘要，保留旧 base，禁止原地替换。
base 中不得加入本仓库的 Core/DI 库或 Python extensions；这些由 NDNSF builder 在同一 base 中构建。

base 验收包含实际 C++ 编译/链接/运行、Rust 编译/运行、依赖 ELF 闭包、Python/NumPy/NDN smoke，
并绑定源码、工具链、headers、库及 package 身份。缺依赖回到 base 补齐，NDNSF 阶段不静默 apt install
或联网补依赖。Cargo.lock 等应用声明与 base 的 vendored 依赖应一致；原生 Rust bridge 仍属于 NDNSF。

## Evidence And Execution

base SDK PASS → NDNSF container build → C++ YOLO runner → 本地真实 MiniNDN 请求链 → 同候选 Tiger。
只运行当前授权范围；前一步结果不替代后一步。SDK 正在实现时保持 PARTIAL。
仓库变更使用原有目标的增量构建；基于同一 base 的宿主驱动构建不允许注入宿主 `.so`/venv。
既有多阶段 Docker/Apptainer builder/final 是构建技术阶段，不是额外交付层。
