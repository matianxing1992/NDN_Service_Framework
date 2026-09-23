# Contract: Two-Provider Placement and Execution

**Status**: TARGET / T005–T007 pending; current implementation gaps are tracked in tasks.md.

## Selection and authorization

真实 Core ACK offers 后规划，profile 可约束 [0,14)/[14,28)。
signed Selection/grant 使用既有类型/canonical identity builder，绑定 Provider、
role/range、manifest/material digests、attempt/epoch、plan digest、dependency endpoints。
不注入假 ACK，不以预导出最终 stage 代替选择。

Selection 验证前可读 summary，不能重型 fetch/创建 runner。
无效/stale/未选/range/digest 错误在生产入口拒绝且计数为零；
验证后仅读选中原子层和显式 shared tensors，校验再组装。

## Causal event contract

- prepare/Repo commit/READY 先于 request；ACK 先于规划与 Selection。
- Selection/grant 验证先于本请求重型 fetch/runner 使用。
- model material fetch/verify 先于 runner ready；cache hit 可替代重新构造，
  不能跳过本次授权和内容验证。Plaintext immutable assembled entries may be
  retained under one stable system cache root rather than a run directory; a hit
  requires the current role/model/recipe/backend identity and a streaming digest
  check of the actual file. Canonical graph/initializer source reuse uses a separate
  candidate-derived source-cache identity and the same hash/size fail-closed rule.
  Protected grant-bound ciphertext remains request scoped.
- upstream tensor fetch 与 model assembly 可交错。首段使用请求输入，没有 upstream；
  后段执行必须等真实 NDN input 验证完成。
- authorization/runner/input 均 ready 才 execute；前段输出先于后段消费；
  末段响应形成 terminal，所有参与者随后 drain。
- events 绑定 request/attempt/provider/role/plan，不依赖跨进程日志行号总序，
  不要求前段 Provider 另发末段 terminal。

当前 `NDNSF_DATA_V1` V3 实现的 deadline 边界：consumer 首次获取 producer 的
placement-bound manifest 属于 producer-readiness wait，受 request hard deadline
和 dependency fetch budget 共同约束；manifest 到达后，各 segment fetch 才使用
`noProgressDeadlineMs` 的 post-publication progress 窗口。该边界不放宽 hard
deadline、terminal/cancel 或 manifest 校验，也不构成真实 Qwen/MiniNDN 资格证据。

现有 Spec189TwoProviderOracle 的
EXECUTION_ENTERED → DEPENDENCY_FETCH → ASSEMBLY_STARTED 固定序必须修正。
日志缺失是 unobserved，不足以推断代码未执行或 timeout 根因。

## Handoff and output

沿 Core/NDN dependency 数据路径，不走 Python/in-process 旁路。
C++ 校验 endpoint/attempt/model/role/sequence、dtype/shape、错误和取消出口。
固定短输入对照独立 reference 的 shape/finite 及冻结 tolerance/top-token；
digest 仅作身份，不能自证正确。无有效 terminal/drain 不得 PASS。
