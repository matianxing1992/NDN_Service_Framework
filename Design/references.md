# 参考文献与采用范围

## NFD Developer’s Guide

- 作者：NFD Team；技术报告 NDN-0021。
- [官方维护 PDF](https://named-data.gitlab.io/TR-NDN-0021-NFD-dev-guide/ndn-0021-nfd-guide.pdf)。2026-09-07 查阅，77 页；该维护版本首页将 Revision 12 标为 TBD，不当作已发布的稳定修订。
- [官方 Revision 11 发布入口](https://named-data.net/publications/techreports/ndn-0021-11-nfd-guide/)。
- 本文参考第 1 节总体结构、第 3 节表结构与语义、第 4 节处理流程、第 5.1 节 Strategy API 及扩展说明的组织方式：组件职责、接口触发条件、允许动作、数据结构与使用约束相互关联。
- 本项目的 API 签名、权限、状态机和实现进度来自 NDNSF 源码，不从 NFD 的接口推导。未复制该报告正文、插图或整表。

## 本项目依据

源码文件和定位见 api/inventory.json、四模块 reference、api/contract-map.json 及 source-baseline.json。
架构和运行边界使用 docs/architecture.md、docs/ndnsf-core-app-boundary.md、docs/NDNSF-DI-runtime-workflow.md 与 active Spec 的契约/证据。
源码注释在 API 参考中保留原文作为接口作者说明；中文行为契约单独维护，不将注释自动升级为测试证明。
## R2 改进参考（2026-09-07 核对）

- [Envoy xDS protocol](https://www.envoyproxy.io/docs/envoy/latest/api-docs/xds_protocol.html)：
  TG-01 借鉴按资源增量更新与已应用版本/ACK/NACK 的区分；不把 xDS 传输或认证直接移入 NDN。
- [gRPC Deadlines](https://grpc.io/docs/guides/deadlines/) 和
  [Cancellation](https://grpc.io/docs/guides/cancellation/)：TG-03 借鉴预算传播与应用协作取消边界。
  取消本地调用不代表应用计算被强制中断。
- [NFD Developer’s Guide](https://named-data.gitlab.io/TR-NDN-0021-NFD-dev-guide/ndn-0021-nfd-guide.pdf)：
  BC/TG 采用流程、状态、扩展约束的说明方式。此参考不提供 NDNSF 实现的正确性证据。
