# T006 — Y-N-E 真实 grant 变异构造

> **Current scope correction (revision 5, 2026-09-05)**: T006 当前 partial：以下三种变异直接调用 Python/native verifier，属于 unit/进程内组件检查；尚未通过真实发布/获取到达选定 Provider，不能作为 Y-N-E 网络资格 PASS。
> 当前裁决与下一步以 [audit.md](../audit.md) 为准，以下保留为原始范围记录。

**Layer**: implemented（变异构造器 + runner 聚焦 probe 重写 + R001
吸收）;executed（6 项 Y-N-E 测试全绿、spec181/runner 143 项全绿，
2026-09-05）;无 measured 声明。

Date: 2026-09-05. Source HEAD: commit `d475fc34`
（T006 mutations absorbed）。

## 声称

Y-N-E 构造三种真实 grant 变异（过期、错误收件人、伪造权威签名）,
每种变异到达已实现 verifier（Python + native 双侧）并在授权边界
（装配之前）以 `DI_PROTECTED_GRANT_REJECTED` 家族原因拒绝;合成纪元
异常路径已删除;被撤销/过期纪元的拒绝以保护纪元绑定失败表达。

## 代码实现（implemented）

- `security/grant_mutations.py`（新文件）：`mutate_expired()`（同
  payload + 过期时间戳 + digest/signature 重建）、
  `mutate_forged_authority()`（非权威密钥签名）、
  `verify_mutation_rejected()`（注册原因家族检查;接受即断言失败）。
- `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`：
  `_run_y_n_e_mutations()` 聚焦 probe——固定种子密钥、进程内权威
  签发、三种变异、Python 与 native verifier 逐一断言拒绝
  （EXPIRED/WRONG_RECIPIENT/FORGED_AUTHORITY × python/native 六路）;
  `YN_NEGATIVE_REASONS["Y-N-E"] = "DI_PROTECTED_GRANT_REJECTED"`。
  R001 吸收：`YN_E_UNAVAILABLE_REASON`/`YN_E_UNAVAILABLE_EXIT` 常量
  删除、`_validate_negative_marker` 的 UNAVAILABLE 特例删除、矩阵
  收集器的 unavailable 路径删除（matrix aggregate 恒为 PASS 或
  incomplete 失败）。
- `tests/python/test_spec181_y_n_e.py` 重写为 T006 语义（6 tests）;
  `tests/python/test_spec180_yolo_minindn.py` 的两个 R001 时代矩阵
  测试更新为 T006 语义。

## 测试执行（executed）

```
python3 -m pytest test_spec181_y_n_e.py -q
6 passed
python3 -m pytest test_spec180_yolo_minindn.py test_spec181_y_n_e.py \
  test_spec181_runner_guard.py test_spec181_provider_grant.py \
  test_spec181_native_grant_parity.py \
  test_spec181_controller_readiness.py -q
143 passed
```

覆盖：三种变异构造 + Python verifier 拒绝原因断言（expired /
authentication / signature）;native 侧对三种变异一致拒绝;
正例仍解包;变异被接受时 fail-closed;runner 注册
`DI_PROTECTED_GRANT_REJECTED`;新 PASS marker 接受;合成原因与
UNAVAILABLE marker 拒绝;聚焦 probe 完整通过;probe 记录注册 PASS。

## 吸收关系

R001 的 UNAVAILABLE 路径由本任务删除（真实变异拒绝取代）。

## Verdict

PASS（T006 范围）。三种真实变异双侧 verifier 拒绝、合成路径删除、
R001 吸收完成。MiniNDN 全矩阵重跑由 T005 执行。
