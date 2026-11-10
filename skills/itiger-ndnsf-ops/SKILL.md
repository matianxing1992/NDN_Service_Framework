---
name: itiger-ndnsf-ops
description: Prepare NDNSF SIF delivery and installed deployment, diagnose Tiger SSH, quota, GPU and lifecycle failures, and operate authorized Slurm experiments through maintained tooling.
---

# iTiger NDNSF Operations

## Apptainer Build Version

所有后续NDNSF SIF构建统一使用Apptainer **1.5.3**（基础、完整SIF及使用Apptainer的分层builder）。先核对实际binary/version/hash；不符先升级，不把 `--expected-apptainer 1.5.3` 改成旧版以绕过门禁。登录节点只传输/提交时不决定构建版本；实际计算作业内另记版本，漂移先解决。历史记录不重写，旧候选不自动重新合格。规范来源：`Experiments/TigerCluster/docs/sif-build.md#apptainer-version-policy`；本规则优先于历史Spec示例的版本选择。

## Authority And Ownership

从目标 checkout 执行 `git rev-parse --show-toplevel` 确定 repo root；以下仓库路径均从该 root 解析，不能从个人安装的 skill 目录推导仓库位置。
先读适用的 `AGENTS.md`；若存在 feature 指针，核对其 plan/tasks，并读取与本任务相关的
failure log 和持久 evidence。缺少可选历史文件不阻断无关工作。当前用户授权优先；
历史 Spec、job ID 或成功候选不是新运行指令。

开发机负责开发、源码审查、unit/integration 和约定的本地 MiniNDN；实验机接收明确源码版本，负责 SIF 构建、Tiger 运行和问题反馈。
仅整理或交付源码时，只整理、固定和发布输入；不要自行启动宿主或容器编译、unit/integration、MiniNDN或集群测试。发现ABI变化时写清接收方重建要求，不把本机补测变成交付条件。不要按 Qwen/YOLO 划分长期机器职责。
已有授权直接完成；缺少运行授权时，先完成可审阅的 definition、输入清单、profile 和确切命令。

Tiger 登录节点仅用于轻量编辑、准备/提交作业和状态查询。模型导出、推理、
镜像构建及大规模数据扫描/完整摘要校验须通过 Slurm 在计算节点执行，包括交互运行。
SSH 故障公告、恢复检查和跨作业存储判断见下方 portable-deployment 参考。
用户要求的容器内推理及生产协议使用 C++ 原生应用；Python 可在宿主侧做配置、
提交和离线分析。对确需交付的 Python 绑定仍须核验容器内构建及 ABI，
不因此把 Python 变成容器内推理依赖。

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
| installed 配置和生命周期 | `NDNSF-DistributedInference/ndnsf_distributed_inference/ops/`；维护的 `ndnsf-di cluster` 入口 |

复用这些入口；不要另建构建 pipeline、复制启动器或把兼容链接变成第二份实现。
先检查真实脚本参数和 effective config，再使用调用命令；不凭本 skill 假设已有工具支持新选项。

处理当前安装、SIF复用、Slurm部署、GPU、配额或SSH故障时，读取
[portable-deployment.md](references/portable-deployment.md)。该参考提炼最新实验中的决策，
不把某次模型、节点、账号、镜像号或资源预算设为默认值。
每次申请新的Slurm allocation前，必须先查询全群资源和自己的在途作业，再选择节点；
这覆盖CPU/GPU推理、模型准备、构建、诊断和独立oracle，也覆盖顺序脚本的后续提交。
遵循该参考的统一提交前资源检查、排队/换节点及600秒复查规则，复用镜像与持久模型。
资源按本次角色的真实峰值估算，不照抄历史请求：例如 Qwen3-4B 两个 CPU Provider，
每个 Provider 只需约8GB内存（其模型半区约4.4GB权重加工作集），整节点约32GB即可；
沿用旧的64GB请求只会降低 backfill 机会并占用无关容量。排不上时先按需求下调内存和
walltime，再考虑换节点，不必无限等待；下调只去掉余量，不能低于已核实的峰值。
Spec170/175参考仅在恢复对应历史工作流时读取；其中的旧镜像、固定profile、
本地构建限定和checklist不自动适用于当前选定部署。当前host管理只支持一个local节点；
Slurm多节点路径不能被描述为通用SSH跨真机启动器。

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
- 模型、输入、token数、节点或Provider布局等已支持配置变化，更新配置/运行身份并复用
  同一合格镜像。先定位实际消费端：只有容器内运行闭包改变才重建SIF；
  容器外host控制工具修复可独立发布wheel并重验其生命周期，不能据此声称镜像内副本已更新。
- base 验收覆盖真实 C++ compile/link/run 和动态加载路径；交付 Python 扩展时另核对其 ABI/import；仅 NumPy/NFD smoke
  不证明 SDK 闭包。base 通过后才构建 NDNSF 候选，再做本地 C++ 模型/请求链与规定绑定验收。
  base PASS、candidate build、YOLO runner、MiniNDN 和 Tiger 结论分别记录，不相互代替。
- 验收最终 base SIF 后按 `docs/base-sif-build.md` 封存镜像、摘要、SDK manifest、输入和真实证据；
  使用仓库外内容寻址目录，文件只读，不纳入临时清理。以后增建生成新目录，不原地覆盖旧 base。

- 固定 NDNSF、NAC-ABE、NDN-SVS 及NDNSD等直接ABI消费者的实际版本；未提交改动须显式纳入来源身份，不能只记录 HEAD。
  交付 definition、source archives、依赖 manifest 与校验说明；所有输入使用可迁移布局和摘要。
- base builder 在容器内构建外部依赖；NDNSF builder 消费同一 base，在容器内编译仓库目标及本次交付所需的 Python 扩展；禁止把宿主 `.so`、venv 或 Python.h 当成容器运行闭包。
  ABI变化后清理依赖对象，核对 include/lib/pkg-config、实际 loaded libraries、SOABI、RPATH 和版本。
- 安装前清理本交付拥有的旧 runtime 输出；已交付的扩展、NAC/SVS/Core与应用二进制进入 manifest。
  final artifact 摘要必须与 builder 记录一致；成功 import 或相同 SONAME 不能证明 ABI兼容。
- base SIF、wheels、模型、tokenizer、adapter、配置、oracle/harness分别标明摘要、提供方式和是否包含。
  外部依赖未提供时写明缺口；根据实际源码和运行闭包报告 Python 依赖，不能仅凭目标架构声称已移除。
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

## Tiger Storage, Cache And Local-Gate Boundaries

- `/project` 的 `df` 不是用户 quota。每种数据只保留最新 SIF/cache/preflight；新候选
  可用后旧候选 SIF（约 4.3GB/个）和旧 cache 按显式 allowlist 回收；峰值按
  cache+spool+assembled 估算，回收后用有界写入探针确认。
- `storage.cache_root` 必须在容器内可见：部署把 `$scratch` 绑到 `/scratch`，宿主
  `/scratch` 会被遮蔽。放在 `/project/...` 或显式 bind 路径，否则 Provider 在
  `IMAGE_ROOT_INSPECTION` 阶段以 `IMAGE_INSPECTION_HOLDER_EXIT:1` 失败。
- 先用 assembled cache 做快速复现；冷组装和 assembled-cache 命中两条 spec 路径都要
  绑定完整 runner 元数据，`warmup` skip 失败与真实执行失败要分开诊断。
- 本地 0.6B/小模型只证明逻辑；4B 的 shape 元数据、容器挂载、quota、request 超时和
  两 Provider 运行时依赖要单独 preflight，不能把 0.6B PASS 当成 4B 部署验证。

按实际证据区分 package checks、build、local runtime、cluster functional 和 performance。
本地两实例只能 LOCAL_PASS；未完成运行不能填写 PASS。结束后同步任务与交接记录，附明确版本、结果、限制和下一步。
