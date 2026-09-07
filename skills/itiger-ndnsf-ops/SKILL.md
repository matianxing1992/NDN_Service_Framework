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
仅整理或交付源码时，不构建 SIF、不提交作业。不要按 Qwen/YOLO 划分长期机器职责。
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

- 固定 NDNSF、NAC-ABE、NDN-SVS 的实际版本；未提交改动须显式纳入来源身份，不能只记录 HEAD。
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
