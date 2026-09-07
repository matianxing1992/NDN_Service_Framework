---
name: ndnsf-minindn-experiment
description: Design, modify, or validate local NDNSF MiniNDN harnesses with explicit topology, real service readiness, independent oracles, process cleanup, and honest measurement boundaries.
---

# NDNSF MiniNDN Experiments

## Repository And Scope

在目标 checkout 用 `git rev-parse --show-toplevel` 确定 repo root；所有下述仓库路径从该 root 解析。
先读 `AGENTS.md`、`.specify/feature.json`、当前任务/契约、`docs/failure-log.md`
和 `docs/architecture-reading-guide.md`。不把旧 Spec 或历史运行作为当前启动指令。
开发机承担约定的本地验证；SIF/Tiger由实验机执行。本 skill不授权额外实验。
按当前计划先完成源码/设计审查和必要单测，正式 integration/MiniNDN遵守 feature gate。

## Topology And Existing Harness

从 `Experiments/` 选择相近维护脚本，拓扑位于 `Experiments/Topology/`。
复用该脚本的生命周期、collector和参数；不要复制出第二套相同启动逻辑。

| Network | Entry / routing |
| --- | --- |
| 默认有线 | `minindn.minindn.Minindn`；沿维护脚本用 NLSR / `NdnRoutingHelper` 或明确静态路由 |
| 明确要求基础设施 WiFi | `minindn.wifi.minindnwifi.MinindnWifi`；核对当前API，按节点IP创建单播 face |
| 明确要求 ad-hoc | `MinindnAdhoc`；按既定链路/组播契约显式 face及路由，不将基础设施WiFi规则套用 |

无线组件和方法签名按当前安装/维护脚本核对，不假设每台机器都有相同扩展。
节点、链路、delay/loss、路由namespace和controller/provider/user位置进入运行记录。

## Launch And Cleanup

- 用当前 MiniNDN 的 `getPopen` 等节点启动接口，确保应用在相应namespace执行；不要用宿主NFD代替真实拓扑验收。
- 每角色独立 HOME、PIB/TPM和证书；记录实际 native 库/绑定来源，排除宿主环境注入。
  必须以实际构建前缀/工具链闭合为准；特权启动 PATH 包括系统管理工具路径。
- 每个应用保留 stdout/stderr 和 owned process句柄。命令/环境参数必须由应用真实消费；未知覆盖拒绝。
  不把某旧实验的 env flags 或 SVS timing 当成通用默认。
- 就绪来自真实路由/服务/权限或签名响应；start返回、固定sleep和端口通都不足以说明可执行。
- 先终止并 wait/reap本run应用，检查残留子进程，再停止/清理MiniNDN；使用finally处理部分启动失败。
  不清理其他用户或其他run的网络/进程。

## Oracles And Evidence

每次用新 output/run-id；保留源码、依赖、工件、有效配置和topology身份。
明确 request/ACK/Selection/Response 等生产事件和独立 payload/数值oracle。
权限、签名、版本、deadline等负例必须命中具名拒绝原因；超时或任意崩溃不是负例成功。
缺事件、缺进程结果、collector失败或清理不完整不能 PASS。

保留有界的快速 smoke 路径，覆盖真实就绪和最小业务行为；预算按实际契约设置，不为满足任意秒数放宽门禁。
smoke只证明其实际路径，不产生吞吐/时延结论，不替代要求的正式矩阵。
失败先记录首边界：preflight/启动失败与协议失败分开；读日志后只修复并重跑受影响范围。

## Measurement When Requested

性能实验固定输入/拓扑/源码/资源/窗口，仅改变被研究变量；每次输出分开。
明确预热、收敛判据、稳态窗口、采样率、重复次数和统计方法；使用当前实验契约，而非通用固定时长。
从结构化事件读取匹配request的时间，不将不同节点未校准时钟直接相减。
保留样本数量、失败/丢失样本和分位数定义；没有有效样本就是缺失结果。

结束时按当前 Spec 同步 tasks/evidence：真实命令、检查结果、完整与部分验收边界、限制和下一步。
