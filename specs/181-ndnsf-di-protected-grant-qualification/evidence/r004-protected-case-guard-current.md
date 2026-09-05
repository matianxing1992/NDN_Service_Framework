# R004 — 保护纪元子用例门禁（Protected-Epoch Case Guard）

> **Current scope correction (revision 5, 2026-09-05)**: 历史 R004 的 false/true 单元预演不证明生产接线；当前 GRANT_WIRING_AVAILABLE=True 及 Python Y-B 路径不关闭 T002 native 验收。
> 当前裁决与下一步以 [audit.md](../audit.md) 为准，以下保留为原始范围记录。

**Layer**: implemented（runner guard）+ executed（7 项 unit 测试全绿，
2026-09-05）;无 measured 声明。

Date: 2026-09-05. Source HEAD: `286a0098` plus the recorded spec181 Phase-0
worktree changes.

## 声称

runner 的 Y-B 保护纪元子用例在 grant 接线（T001/T002）完成前必须失败
关闭（`DI_PROTECTED_GRANT_UNAVAILABLE`），不得以明文路径冒充保护纪元
执行、不得产出 PASS 记录。门禁在任何 output-root/validation 副作用前
生效。T001/T002 吸收 commit 将门禁翻转（`GRANT_WIRING_AVAILABLE = True`）
转为正常执行。

## 设计依据

对 runner 全部输入 seam 的核查（2026-09-05）：case config roles、
case plan roles、manifest role rows（{kind, role} only）、policy loader
ServicePolicy 均不存在结构化 epoch request seam——runner input chain 中
没有可承载保护纪元声明的字段。因此采用与 runner 既有
`SPEC180_YN_MUTATION` env 约定同构的 env 通道
`SPEC181_PROTECTION_EPOCH` + module 级开关 `GRANT_WIRING_AVAILABLE`
声明执行请求的纪元;门禁独立于未来的具体封印纪元值（非 plaintext 即
拒绝，直到 wiring 落地），吸收 commit 无需改动 env 契约。

## 代码实现（implemented）

`Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`:

- 常量（:256-259）：`GRANT_WIRING_AVAILABLE = False`（T001/T002 吸收
  commit 翻转为 True）;`PROTECTION_EPOCH_ENV = "SPEC181_PROTECTION_EPOCH"`;
  `PLAINTEXT_EPOCH = "plaintext-v1"`（= RoleAssemblySpec 默认值）。
- `_assert_grant_wiring_or_plaintext(environment)`（:262-）：未声明或
  `plaintext-v1` → 放行;非 plaintext 且 `GRANT_WIRING_AVAILABLE` →
  放行（wiring 已落地，转真实 verifier）;否则 raise `RunnerError`
  `DI_PROTECTED_GRANT_UNAVAILABLE: protected-epoch execution requested
  (<epoch>) but grant wiring is not implemented (spec181 R004; T001/T002)
  - refusing to run the plaintext path as a protected epoch`。
- `main()`（:3233）：parse args 后第一动作即调用该 guard;拒绝时打印
  `SPEC180_CASE_RESULT status=UNQUALIFIED error=DI_PROTECTED_GRANT_...`
  （:3235）并 exit 2——先于 validate_inputs（:78-exit 路径）、native
  closure 与 MiniNDN 启动，零 output-root/validation 副作用。
- spec180-era plaintext 运行不受影响：env 未设置时 guard 直接放行。

## 测试执行（executed）

`tests/python/test_spec181_runner_guard.py`（7 tests，module 经
`importlib.util.spec_from_file_location` 加载）:

1. 无纪元声明 → 放行;
2. 显式 `plaintext-v1` → 放行;
3. `spec180-yolo-protected-v1` → refuse `DI_PROTECTED_GRANT_UNAVAILABLE`;
4. 未知纪元（`epoch-yet-unsealed`）→ 同样拒绝（门禁不依赖未来封印值）;
5. `GRANT_WIRING_AVAILABLE = True`（吸收翻转）→ 保护纪元放行;
6. `main()` 在保护纪元 env 下返回 2、输出 UNQUALIFIED +
   `DI_PROTECTED_GRANT_UNAVAILABLE`、validate_inputs 未被调用
   （monkeypatch 断言）;
7. plaintext `main()` 仍到达 validate_inputs（exit 78 路径正常）。

结果：`python3 -m pytest tests/python/test_spec181_runner_guard.py -q`
→ 7 passed（与 test_spec181_y_n_e.py、test_spec180_yolo_minindn.py 合跑
共 96 passed，见 r001 evidence）。

## 吸收关系

T001/T002 落地后吸收 commit 将 `GRANT_WIRING_AVAILABLE` 翻转为 True：
保护纪元请求进入真实 verifier 路径，门禁不再拒绝。届时更新本文件。

## Verdict

PASS（R004 范围）。未接线时保护纪元用例被拒且原因明确（unit 层
验证）;无明文冒充、无 PASS 产出;吸收翻转已用 unit 预演。
