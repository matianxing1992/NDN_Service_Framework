---
name: itiger-ndnsf-ops
description: Prepare or review NDNSF source and SIF delivery, diagnose container ABI and launch boundaries, and operate authorized Tiger experiments through the repository's maintained tooling.
---

# iTiger NDNSF Operations

## Apptainer Build Version

所有后续NDNSF SIF构建统一使用Apptainer **1.5.3**（基础、完整SIF及使用Apptainer的分层builder）。先核对实际binary/version/hash；不符先升级，不把 `--expected-apptainer 1.5.3` 改成旧版以绕过门禁。登录节点只传输/提交时不决定构建版本；实际计算作业内另记版本，漂移先解决。历史记录不重写，旧候选不自动重新合格。规范来源：`Experiments/TigerCluster/docs/sif-build.md#apptainer-version-policy`；本规则优先于历史Spec示例的版本选择。

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

## Delivery And Native Boundary

当前交付采用两层：**base SIF + NDNSF**。base 包含全部已声明外部运行依赖及构建 SDK；
NDNSF 层统一拥有本仓库的 Core、DI、Repo、UAV、绑定和实验入口，不再另划第三个 APP 层。
旧脚本的 APP 字段/路径可作为兼容名称，但不能据此重复构建外部依赖。

- 先检查现有 base 的摘要、依赖 manifest、头文件/库/pkg-config、编译器和 Python ABI。
  缺依赖时以现有已验证 base 为父镜像，补齐后输出新 SIF 与新摘要，保留旧镜像；不默认从零重建。
- ONNX/ORT、Protobuf、Rust、NAC-ABE、NDN-SVS、NDNSD 等外部依赖在 base 中准备并验证。
  按消费者实际需求核对完整依赖集合；不把这份举例当穷尽清单。版本、编译选项或 ABI 变化才使 base 失效。
- NDNSF 日常源码变化只重建受影响的仓库目标、绑定及相应验收；不在 NDNSF 层 apt install、
  下载依赖或再次编译外部库。Cargo.lock 等依赖声明仍随源码封存，需与 base 的依赖集合匹配；
  缺项回到 base 补齐，不由构建阶段静默联网解决。
- base 验收覆盖真实 C++ compile/link/run、Python ABI/import 和动态加载路径；仅 NumPy/NFD smoke
  不证明 SDK 闭包。base 通过后才构建 NDNSF 候选，再做本地 C++ 模型/请求链与规定绑定验收。
  base PASS、candidate build、YOLO runner、MiniNDN 和 Tiger 结论分别记录，不相互代替。
- 验收最终 base SIF 后按 `docs/base-sif-build.md` 封存镜像、摘要、SDK manifest、输入和真实证据；
  使用仓库外内容寻址目录，文件只读，不纳入临时清理。以后增建生成新目录，不原地覆盖旧 base。

- 固定 NDNSF、NAC-ABE、NDN-SVS 及NDNSD等直接ABI消费者的实际版本；未提交改动须显式纳入来源身份，不能只记录 HEAD。
  交付 definition、source archives、依赖 manifest 与校验说明；所有输入使用可迁移布局和摘要。
- base builder 在容器内构建外部依赖；NDNSF builder 消费同一 base，在容器内编译仓库目标和 Python 扩展；禁止把宿主 `.so`、venv 或 Python.h 当成容器运行闭包。
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
