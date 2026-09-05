# R001 — Y-N-E 诚实化（UNAVAILABLE until T006）

> **Current scope correction (revision 5, 2026-09-05)**: 历史 R001 的 unit 结果保留。当前 T006 的进程内 probe 尚未满足生产变异验收，其 PASS 不得晋升为网络安全证据。
> 当前裁决与下一步以 [audit.md](../audit.md) 为准，以下保留为原始范围记录。

**Layer**: implemented（runner + user.py + unit tests）;executed（96 项
Python unit 测试全绿，2026-09-05）;无 measured 声明。

Date: 2026-09-05. Source HEAD: `286a0098` plus the recorded spec181 Phase-0
worktree changes.

## 声称

在真实 grant 变异（T006）落地前，runner 的 Y-N-E 子用例只允许报告
`UNAVAILABLE`（结构化原因 `Y-N-E:GRANT_VERIFIER_NOT_IMPLEMENTED`），
禁止：合成纪元异常充当 `PROTECTION_EPOCH_REJECTED`、任何 PASS 记录、
以及 verifier 从未进入却声称越过授权边界的生命周期。合成拒绝路径已
删除。

## 代码实现（implemented）

### Runner — `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`

- 常量（:244-248）：`YN_E_UNAVAILABLE_REASON = "Y-N-E:GRANT_VERIFIER_NOT_IMPLEMENTED"`、
  `YN_E_UNAVAILABLE_EXIT = 93`;`YN_NEGATIVE_PASS_EXIT` 仍是 91（Y-N-O/
  Y-N-A/Y-N-R 的 registered PASS exit），Y-N-E 不得使用。
- 子用例分发（~:2782 `_run_focused_y_n_negative`）：Y-N-E → 无合成
  probe;写 subcase-result `status="UNAVAILABLE"`,
  `outcome="FAIL_CLOSED_UNAVAILABLE"`, `reason=YN_E_UNAVAILABLE_REASON`,
  `expected="FAIL_CLOSED"`（:2805, :2830）。
- 等待/校验（:2930, :3068, :3087）：对 Y-N-E 只接受子进程 exit 93
  （UNAVAILABLE）;`_validate_negative_marker` 对 E 拒绝
  `status=PASS`、拒绝非结构化原因（如旧 `PROTECTION_EPOCH_REJECTED`）、
  允许的 observedPhase 只到 `ARTIFACTS_READY`（verifier 从未进入，不得
  越过授权边界）;违者 raise `Y_N_NEGATIVE_MARKER_INVALID` /
  `Y_N_NEGATIVE_LIFECYCLE_MISMATCH`（:3174）。
- 矩阵聚合（:3215）：存在 unavailable 记录时 raise
  `Y_N_MATRIX_Y_N_E_UNAVAILABLE` 并写 matrix aggregate
  `UNQUALIFIED_GRANT_VERIFIER_NOT_IMPLEMENTED`——UNAVAILABLE 不进
  PASS 计数。

### User child — `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py`

- 边界表（:60-63）：`_YN_NEGATIVE_BOUNDARIES["Y-N-E"] = ("ARTIFACTS_READY",
  "Y-N-E:GRANT_VERIFIER_NOT_IMPLEMENTED")`;`_SPEC180_Y_N_E_UNAVAILABLE_EXIT = 93`（:68）。
- PASS 禁止（:128-131）：Y-N-E mutation 走到 PASS emitter →
  raise `RuntimeError("SPEC180_Y_N_E_PASS_FORBIDDEN")`。
- 绑定 seam 诚实失败（:176-180）：post-ACK 绑定处对 Y-N-E 直接 raise
  `RuntimeError("SPEC180_Y_N_E_GRANT_PATH_UNAVAILABLE")`——不构造合成
  grant 拒绝（删除原 synthetic `PROTECTION_EPOCH_REJECTED` 路径）。
- UNAVAILABLE emitter（:142-151）：打印
  `SPEC180_YN_NEGATIVE_RESULT status=UNAVAILABLE subcase=Y-N-E
  boundary=ARTIFACTS_READY reason=Y-N-E:GRANT_VERIFIER_NOT_IMPLEMENTED
  requestId=... attemptId=... observedPhase=...`（observedPhase 动态，
  绑定 seam 实际到达的里程碑，≤ ARTIFACTS_READY）。
- run() 异常路径：仅当异常匹配 `_spec180_y_n_e_unavailable_match`
  （:111-121：RuntimeError + 精确消息 + phase ∈ MILESTONES_PRE_ARTIFACTS，
  :200-203）才 emit UNAVAILABLE 并 exit 93;否则 re-raise。
- 生命周期：Y-N-E 观察点限 `MILESTONES_PRE_ARTIFACTS`（≤ ARTIFACTS_READY）。

## 测试执行（executed）

`tests/python/test_spec181_y_n_e.py`（8 tests）+ 两个更新到 R001 语义的
`tests/python/test_spec180_yolo_minindn.py` 矩阵测试。命令与结果：

```
cd /home/tianxing/NDN/ndn-service-framework/tests/python
python3 -m pytest test_spec181_y_n_e.py test_spec180_yolo_minindn.py \
  test_spec181_runner_guard.py -q
96 passed
```

覆盖：exit 93 marker accepted;早相位（GRAPH_READY）accepted;PASS marker
拒绝;合成 reason 拒绝;越过 PLAN_SEALED 拒绝（生命周期不匹配）;user
exit 91 视为 child failure;focused probe 写 UNAVAILABLE/FAIL_CLOSED_
UNAVAILABLE 到 `subcases/Y-N-E/subcase-result.json`;矩阵驱动下
unavailable 记录不进入 PASS 计数（矩阵 raise
Y_N_MATRIX_Y_N_E_UNAVAILABLE + aggregate
UNQUALIFIED_GRANT_VERIFIER_NOT_IMPLEMENTED）。

## 吸收关系

T006 落地真实 grant 变异后删除 UNAVAILABLE 路径，由真实变异拒绝取代
（flip 点在吸收 commit;届时更新本文件为历史记录）。

## Verdict

PASS（R001 范围）。Y-N-E 在 verifier 缺席期间诚实报告 UNAVAILABLE;
无合成拒绝、无 PASS 冒充、无越界生命周期。
