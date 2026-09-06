# Qualification Inventory Scope Repair

**Status**: PASS (focused inventory repair); T007 BLOCK
**Evidence layer**: implemented / wired / executed (focused RED and GREEN)

## First Boundary

A05 调用链核对发现 `scripts/spec180_inventory.py` 的 `DEFAULT_CASES`
仍要求 Q-C/Q-W，`discover_python_selectors` 却只发现
`test_spec180_*.py`；`run_spec180_local_gate.py` 又独立硬编码五案例。
Spec181 T008 要求 YOLO Y-A/Y-B/Y-N，且必须覆盖 Spec181 新增保护
grant 回归。旧清单同时扩大模型资格范围并漏掉活动 Spec 测试。

两个新增测试调用实际 `build_inventory` 与 pytest collection，使用
临时文件作为候选/测试输入。R1 **2 failed（0.53s）**：生成清单
包含 Q-C/Q-W；真实 collection 缺少 `test_spec181_protected_fixture`。
原始日志位于 ignored workspace temporary directory 的
`spec181-inventory-scope-20260905-r1/red.log`。未启动正式网络资格。

## Repair Boundary

维护路径和既有记录字段保持兼容；活动案例集合收口至三个 YOLO
入口，生成器和执行器共用这一集合。保留继承 Spec180 单元回归，
同时收集 Spec181 回归；旧含 Q-C/Q-W 的资格清单明确拒绝。
模型资格边界不影响 Qwen 自身的生产入口或共用接口定向测试。
通过本修复仍不代表 A05 的全部源/配置闭包或 T007 已完成。

R2 三个定向测试文件共 15 PASS / 1 FAIL（2.64s）：两个新增范围
回归已通过，模拟子进程 gate 已 PASS；唯一失败是旧测试仍预期
8 个条目，实际新范围为 3 个单元/集成条目加 3 个 YOLO 入口，共 6。
保留 R2 日志，修正这一范围相关的旧断言后在新 R3 重跑。

R3 三个文件 **16 PASS（2.81s）**。继续核对源绑定后，R4 新增
两项回归 **2 FAIL（0.81s）**：实际 builder 接受把 Y-B 绑定到另一
脚本；实际 gate 接受重新计算全部摘要的 Y-A 脚本替换并启动 fixture
子进程。注册案例名、路径与参数没有共同契约检查，单纯摘要自洽
不能证明执行了注册入口。这里的子进程为显式轻量 fixture，无网络
资格结果；下一步在 builder/validator 公共边界拒绝该替换。

R5 为 17 PASS / 1 FAIL（2.10s）：两个重算摘要替换回归已拒绝。
旧“缺 oracle”测试通过更换案例路径构造 fixture，现在在正确的
更早契约边界被拒绝。改为保持注册路径、只让 Y-A fixture 不输出
oracle，保留该独立检查目的；R5 原始结果保留后再执行 R6。

R6 **18 PASS（2.89s）**。继续核查输出目录时，发现 entry ID 只
要求非空/不重复，gate 却直接以其拼接 evidence directory。R7
针对 `../outside`、绝对路径和 `..` 的三个纯 validator 回归
**3 FAIL（0.18s）**；未写越界文件。下一步限制为单个安全 ID 分量，
在启动任何子进程前拒绝越界 ID。

## Final Focused Validation

R8 `test_spec181_inventory_scope.py`、`test_spec180_inventory.py`、
`test_spec180_local_gate.py` 共 **21 PASS（2.45s）**，exit 0。
实际 generator/validator/gate 均使用同一注册案例契约；Spec180 与
Spec181 Python 回归都被发现，旧 Q-C/Q-W 清单被拒绝，重新计算
摘要的案例脚本替换和越界 entry ID 在任何 child/output 创建前拒绝。

既有摘要变异、命令形状、缺 oracle、敏感输出处理、非空输出目录、
逐子进程快照及 Qwen wrapper 输出路径接口检查继续通过。受监督
子进程是临时 fixture，Python discovery 调用真实 pytest collection；
没有执行正式 MiniNDN、Qwen 模型、SIF 或 Tiger 资格。

原工作区未跟踪的两个维护工具及其测试作为同一已检查的工具单元
收口；不把工作区其他代码纳入本单元。A05 的 native/candidate
有效配置及封印输入闭包仍需继续核查，本记录不关闭 T007。
