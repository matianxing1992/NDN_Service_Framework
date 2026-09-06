# T008 Local Suite Preflight Review

**Date**: 2026-09-06 | **Task**: T008
**Evidence layer**: proposed / implemented (source inspection only)
**Status**: BLOCK (complete local gate configuration; T005 R19 subject unchanged)

## Scope And Source

本次核对 `ce6a4ba0f07bbdbc954a667f7846e5348c8861da` 隔离源码与主工作区。
T005 R19 使用该隔离源码运行 Y-N，自己的五角色配置已通过预检；
本记录不改变其源码、配置或既有执行前审计。这里发现的是 T008
将 Y-A/Y-B/Y-N 放入同一 supervised local-suite 时的配置缺口。

## Case Configuration Gap

`scripts/run_spec180_local_gate.py::_run_entry` 复制一份公共 environment，
仅为 MiniNDN 子进程覆盖 `SPEC180_CASE_OUTPUT_DIR`。
`scripts/spec180_inventory.py::DEFAULT_CASES` 固定三个案例；
`Experiments/NDNSF_DI_YoloAckDriven_Minindn.py::_validate_case_config`
要求每个案例的角色集合精确匹配：Y-A 为 FullModel，Y-B 为四个共享
角色，Y-N 为这两者并集。单一 `SPEC180_YOLO_CONFIG` 无法满足三者，
不能以删掉案例、放松角色检查或把分别启动的结果拼成完整 gate 修复。

### Planned Repair

保持现有 runner 的参数和严格角色校验不变。在本地 gate 的公共
配置中声明三个独立配置路径：`SPEC181_LOCAL_CONFIG_Y_A`、
`SPEC181_LOCAL_CONFIG_Y_B`、`SPEC181_LOCAL_CONFIG_Y_N`。

| Owner / symbol | Intended change |
|---|---|
| `scripts/spec180_inventory.py::CASE_CONFIG_ENV` | 单一 case → 配置变量映射，供清单和 supervisor 共用 |
| `local_launch_configuration` | 三项全有或全无；部分声明拒绝。按案例配置启用时拒绝同时声明公共 `SPEC180_YOLO_CONFIG`，避免双重来源 |
| `local_input_identity` | 对三份配置的实际文件内容、路径与 mode 绑定；修改任一文件使输入摘要失效 |
| `scripts/run_spec180_local_gate.py::_run_entry` | 仅对对应 MiniNDN case 将所选路径传入 `SPEC180_YOLO_CONFIG`；测试进程不接收虚构 case 配置 |
| `tests/python/test_spec180_inventory.py` | 不完整声明、双重来源、配置内容漂移与完整绑定的定向断言 |
| `tests/python/test_spec180_local_gate.py` | 通过真实 supervisor 子进程观察三个案例收到各自配置；缺失/漂移在执行边界失败，不靠 PASS 标签证明配置正确 |

未声明三项时保留原公共配置机制，供已有单一配置调用者使用；本轮
完整 MiniNDN gate 必须使用三项。预检、输入身份与子进程配置共享同一
映射。三项配置由已有案例契约生成或取得，不让 gate 推导授权角色。
输入路径须可解析；秘密仍在既有私有 key map 中，配置只引用身份。

修复后先定向回归并复审，再从新的明确提交运行完整 T008；R19 旧源
证据仍保留，不能替代新源的 Y-N 重跑。测试超时按已有案例各阶段的
有限预算和九次运行（E 含三变异）安排，不以放宽业务 deadline 修复。

## Test Source Closure

`discover_python_selectors` 按 `test_spec180_*.py` 与 `test_spec181_*.py`
发现所有文件；主工作区有 52 个，隔离源码有 33 个。以下 19 个尚未
纳入该提交，不能仅凭隔离目录较小就声明所有预存测试已处理：

| Group | Missing test suffixes after `test_spec180_` | Review obligation |
|---|---|---|
| Shared runtime / application | ack_provenance, negative_verdict, role_assembly, yolo_application, yolo_equivalence, yolo_security, yolo_numerical | 核对真实生产调用与测试依赖，决定纳入交付或给出明确不适用依据 |
| Candidate / local inputs | candidate, contract_gate, dispatcher, yolo_adapter, yolo_export | 相应 helper/exporter 尚未提交，须检查依赖闭合，不能只复制测试 |
| Existing Qwen contracts | qwen_entrypoint, qwen_reference | 区分共用接口回归与范围外模型资格，不把测试名当作资格结论 |
| Experiment tooling / native evidence | native_evidence, terminal_collector, release_workflow, tiger_contract, tiger_supervision | 核对本地契约检查用途与移交范围；不启动 SIF/Tiger 实验 |

`spec180_candidate.py`、`spec180_contract_gate.py`、`run_spec180_case.py`、
`spec180_release.py`、`validate_spec180_results.py`、YOLO exporter 与
experiment collector/supervisor 当前仍为主工作区未跟踪文件，隔离源码
不含这些文件。native-evidence 的两个 C++ fixture 也未纳入。
此处是来源差异清单，尚未对这些文件作采用或排除裁决。

主工作区另有尚未采用的 tensor manifest unit-test 草稿，其中默认
legacy encoder 的大包断言与最终契约不符；不能原样纳入或为迁就测试
改变已验证的 legacy 格式。真实紧凑传输已由 R6 九个用例覆盖。

完整 unit/integration 二进制在隔离构建中尚未产出。必须先闭合源与
构建注册，再发现完整选择器并逐项监督执行；不得缩短 selector 清单
或将 focused 二进制更名当作完整 suite。

## Next Action

先完成并记录 T005 R19 的实际终态；按上述设计修复 T008 case 配置，
完成测试来源处置与构建清单，再进行同源完整资格验证。T008 未完成。
