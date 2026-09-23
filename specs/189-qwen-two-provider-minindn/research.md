# Research: Qwen Two-Provider Full Path

## Decision: preparation separate from placement

prepare 负责拓扑无关原子模型材料和 Repo 可达性；ACK 后规划分区、
Selection 后组装最终 ONNX。预导出两段只可作对照，不能替代真实链。

## Decision: close the existing producer and consumer

Repo adapter layer payload/冷热 fixture 不代表真实 requester 已接线。
canonical assembler 整 initializer 读取必须改为消费同一 Repo 选定材料，
否则磁盘缓存不能消除 Provider 的整模型副本。
不另建 Qwen API、serializer 或隐式临时 fallback。

## Decision: reuse component evidence

保留全局 dependency closure 和已有 C++ checks。
T002/T004 合并 T003、T008/T010 合并 T009；资源门保留为每次 full-model run
的跨批次前置条件和最终 drain 判据，不再作为独立能力任务。

## Local-detail boundary

Qwen layer map、material manifest 的候选字段、profile 阈值、MiniNDN topology
和 Spec189 oracle 只服务本地候选。它们不能反向修改 NDNSF Core/Repo 的全局
默认值或公共 API；只有真实跨模块契约变化才进入 Design/MANAGEMENT.md 规定的
独立设计变更。native C++ 是 source identity 和 prepare 的权威实现，Python
helper 只做预检和编排。
组件与两 Provider 正式资格分开，避免字段级任务/重复编译 executable。

## Known boundary

r25 为 FAIL，已观察 assembly entry，旧数据路径仍有 di-canonical-initializer。
stream gap 不足以确定根因。本轮未测新峰值；12 GB 是否足够须由有界材料化后的
真实运行证明。见 [audit](evidence/spec189-static-audit-20260918.md#architecture-and-progress-correction)。
