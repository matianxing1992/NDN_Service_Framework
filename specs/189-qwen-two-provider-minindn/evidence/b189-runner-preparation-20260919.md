# B189 Native runner preparation checkpoint

本批次覆盖 T006 的一个独立边界：Provider 在 authenticated Selection projection
之后绑定 generation state 和 position metadata。`NativeRunnerPreparation.cpp` 现在
先清除 projection-owned 的 successor/position 字段，再按 projection 写回；因此
adapter 预填值不能覆盖空的 legacy contract，也不能在同一 spec 复用时残留。

官方只读 `review-agent` 复审结果为 `STATIC_PASS`。审查确认空 successor map 回退
legacy `_in/_out` 语义，非空 map 继续执行格式、重复、跨 role 和覆盖数量检查；
position policy 继续要求字段存在于选定 role 的输入边界。

验证：

- global-r3 `unit-tests` 目标使用 `-j1` 编译通过，耗时 `28.600s`；
- `NativePreparationContext*` C++ selector：`3` cases，`No errors detected`；
- 新增反例覆盖 stale `stateSuccessorMap`/`kvTensorMap`/position metadata 清理，
  以及非空 authenticated successor/position 写回。

这只是 metadata binding 的 C++ 单元证据，不证明真实 ORT runner、Provider ingress、
ACK/Selection 或两 provider MiniNDN 资格；T006/T009 仍保持 `PARTIAL`。
