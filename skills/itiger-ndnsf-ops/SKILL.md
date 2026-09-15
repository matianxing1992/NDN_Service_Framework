---
name: itiger-ndnsf-ops
description: Prepare or review NDNSF source and SIF delivery, diagnose container ABI and launch boundaries, and operate authorized Tiger experiments through the repository's maintained tooling.
---

# iTiger NDNSF Operations

## Authority And Ownership

从目标 checkout 执行 `git rev-parse --show-toplevel` 确定 repo root；以下仓库路径均从该 root 解析，不能从个人安装的 skill 目录推导仓库位置。
先读 `AGENTS.md`、`.specify/feature.json`、当前 feature 的 plan/tasks、`docs/failure-log.md`
与相关持久 evidence。当前用户授权优先；历史 Spec、job ID 或成功候选不是新运行指令。

开发机负责开发、源码审查、unit/integration 和约定的本地 MiniNDN；实验机接收明确源码版本，负责 SIF 构建、Tiger 运行和问题反馈。
仅整理或交付源码时，只整理、固定和发布输入；不要自行启动宿主或容器编译、unit/integration、MiniNDN或集群测试。发现ABI变化时写清接收方重建要求，不把本机补测变成交付条件。不要按 Qwen/YOLO 划分长期机器职责。
已有授权直接完成；缺少运行授权时，先完成可审阅的 definition、输入清单、profile 和确切命令。

## Repository Routing

| Work | Read / reuse from repo root |
| --- | --- |
| 目录、所有权和兼容路径 | `Experiments/TigerCluster/README.md` |
| SIF 构建与输出 | `Experiments/TigerCluster/docs/sif-build.md`、`packaging/ndnsf-di-container/README.md` |
| 三库固定与可迁移输入 | `Experiments/TigerCluster/docs/source-handoff.md`、`Experiments/TigerCluster/development-handoff.lock.json` |
| 已有构建/校验入口 | `Experiments/TigerCluster/adapters/slurm-apptainer/scripts/`、共享 `Experiments/TigerCluster/lib` 和 `bin` |
| 双节点基础设施与复用消费者 | `Experiments/TigerCluster/docs/two-node-baseline.md`、`runtime/`、`apps/`、`jobs/baseline/`、`profiles/`（均位于 TigerCluster 下） |
| 模型准备与证据 | `Experiments/TigerCluster/docs/itiger-qwen-models.md`、`itiger-qwen-evidence.md`（同一 docs 目录） |
| 既有 workload jobs | `Experiments/TigerCluster/jobs/`；按当前契约选择，不自动启动历史资格流程 |

复用这些入口；不要另建构建 pipeline、复制启动器或把兼容链接变成第二份实现。
先检查真实脚本参数和 effective config，再使用调用命令；不凭本 skill 假设已有工具支持新选项。

## Successful-Candidate Template Gate

每次 Spec186、Spec184 或后续 NDNSF-DI 的 SIF/Tiger 工作，在生成新
definition 或申请计算节点之前，必须先阅读并逐项比对
`Experiments/TigerCluster/docs/successful-tiger-gpu-template.md`。该文件是
最后一次完整 YOLO GPU 候选的**参考模板**，不是新运行的证据；其中的
base-SIF 摘要、Clang 10/O0 编译边界、Apptainer 1.5.3、app/model/oracle
字段和单/双节点验收形状必须进入新候选的比较表。

候选 definition 只能通过维护的
`prepare-development-handoff.py render` 从
`development-runtime.def.in` 渲染。不得从失败 definition 手工复制、把
intermediate SIF 当成成功模板的 base、将 GCC/Clang/工具链根悄悄替换，或
通过删掉构建步骤来绕过失败。每次允许的差异都要记录为新的 candidate
plane（source、base、app、compiler、profile、model、harness 或 resource），
并在昂贵构建前保留一份 template-vs-rendered diff 和 base/definition/source
摘要交叉检查。

构建前固定顺序：

1. 核对模板冻结元组和实际 base-SIF SHA/bytes；base 不匹配时停止，不用相似
   文件名或中间产物替代。
2. 用 `render` 生成 definition，运行 `bash -n`、嵌入 Python AST、
   `spec170_sif_build_boundary.py` 和 `preflight-development-sif.py`。
3. 检查编译器可执行文件、`--toolchain-root`、CXXFLAGS、Waf child source/
   build pair、pkg-config 和 final-stage `%files from builder`；任何模板漂移
   先记录失败再修复，不能边构建边猜参数。
4. 仅在上述门全绿后调用唯一入口
   `adapters/slurm-apptainer/scripts/build-local-sif.sh`，并将完整原始日志与
   candidate 绑定。构建、import 或 READY 通过仍不能替代 MiniNDN/Tiger
   协议、CUDA、数值和 cleanup 证据。

`preflight-development-sif.py` 还必须检查 definition 的 builder shell 实际
消费的源码路径：凡是被 `cp` 或 pip 安装命令引用的 `/src/ndnsf/...` 路径，都要
在 `workspace.tar` 中存在；否则以 `WORKSPACE_CONSUMER_PATH_MISSING` 在编译前
停止。传入 `--base-sif` 和同一 Apptainer 时，它会从 definition 的 `test -x/-f/-d`
谓词提取 base-owned 工具、ONNX SDK、Rust 和头文件能力并在只读容器中验证。这样
可以在秒级发现“归档存在但实际消费的子目录缺失”和“definition 要求的编译器/SDK
不在 base 中”，而不是等 `%post` 运行到对应命令才失败。

`build-local-sif.sh` 会在 preflight 前解析 `Bootstrap: localimage` 的真实 base
路径，不能再静默退化为 definition-only 检查。若 SIF 已经生成、但后续 ABI 或
运行时门失败，使用同一 definition、source seal、host gate、Apptainer 和输出
路径调用 `--verify-existing`；它会重新验证 label、SIF hash 和完整 Spec175
preflight，并生成 `local-apptainer-existing-sif-verify` 记录，不重新编译。只有
source/base/definition 或候选闭包改变时才删除旧候选并新建 SIF。

历史成功 job、SIF 或 app 可以帮助确定验收形状，但不能直接复用 job ID 或把
旧结果拼入新 candidate。若发现新的构建失败，先将 symptom、root cause、
correction、lesson 写入 `docs/failure-log.md`，并把新的防回归谓词加入脚本、
测试或本节；未完成该记录前不得再次提交同类构建。

成功模板的 base digest 不代表 base 内已经有历史 builder 工具链。提交前必须
在目标计算节点的 exact SIF 中探测 Clang/GCC、ONNX full-protobuf、Rust/Cargo、
Python headers、CMake、protoc 和 pkg-config。若 root-mapped Apptainer 无法运行
Debian `_apt` sandbox，禁止把 apt 删除后继续声称模板复用；应建立单独 source-
sealed 的工具链派生 base（记录 `APT::Sandbox::User "root"`、project-backed
`APPTAINER_TMPDIR` 和新 base SHA），或停止并反馈缺口。严禁把宿主编译器/`.so`
复制进 SIF。

## Delivery And Native Boundary

- 固定 NDNSF、NAC-ABE、NDN-SVS 及NDNSD等直接ABI消费者的实际版本；未提交改动须显式纳入来源身份，不能只记录 HEAD。
  交付 definition、source archives、依赖 manifest 与校验说明；所有输入使用可迁移布局和摘要。
- builder 在容器内重新编译选定依赖、Core 和两个 Python 扩展；禁止把宿主 `.so`、venv 或 Python.h 当成容器运行闭包。
  ABI变化后清理依赖对象，核对 include/lib/pkg-config、实际 loaded libraries、SOABI、RPATH 和版本。
- 安装前清理本交付拥有的旧 runtime 输出；两扩展、NAC/SVS/Core与应用二进制进入 manifest。
  final artifact 摘要必须与 builder 记录一致；成功 import 或相同 SONAME 不能证明 ABI兼容。
- base SIF、wheels、模型、tokenizer、adapter、配置、oracle/harness分别标明摘要、提供方式和是否包含。
  外部依赖未提供时写明缺口；保留当前生产 Python helper，不提前声称完成无 Python 迁移。
- 凭据、私钥、token、个人账户配置不入 Git。公开证书与摘要可记录；每角色独立 HOME/PIB/TPM，不分发根私钥。

## Authorized Execution And Evidence

使用选定 profile 和唯一维护入口渲染完整命令、环境、挂载、路由、资源及预算。
拒绝未知字段、未消费的环境覆盖和身份变化；只有当前契约允许的 delta 可进入运行。
独立 run directory 保存每次首边界、原始日志、退出码和清理；不覆盖失败，不因失败自动改变模型、资源或超时。
传输/提交结果不明时先核对已记录的版本或 job，不重复提交。

就绪必须来自真实服务/认证边界，不能只看进程存在、端口通或 start 返回。
独立 oracle 核对结果及负例原因；超时、缺结果、任意崩溃不能冒充协议拒绝。
所有 owned processes 必须清理；leader 退出不代表其进程组已清理。
失败后先诊断首边界，源/依赖/config/harness变化回到最早失效门；仅重跑受影响范围。

按实际证据区分 package checks、build、local runtime、cluster functional 和 performance。
本地两实例只能 LOCAL_PASS；未完成运行不能填写 PASS。结束后同步任务与交接记录，附明确版本、结果、限制和下一步。
