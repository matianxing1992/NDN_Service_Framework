# T008 Local Suite Preflight Review

**Date**: 2026-09-06 | **Task**: T008
**Evidence layer**: proposed / implemented / wired / executed (focused configuration repair)
**Status**: BLOCK (test-source/build closure pending; case configuration focused PASS)

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

T005 R19 已完整 PASS。T008 case 配置定向修复也已通过（下文 R2）；
下一步完成测试来源处置与构建清单、准备三案例实际配置并复审，再
进行同源完整资格验证。T008 未完成。

## Test Adoption Batch A

先核对并纳入四个已有测试：`test_spec180_ack_provenance.py`（Python
ACK 投影及默认认证门）、`test_spec180_negative_verdict.py`（真实
User 异常分支，拒绝无关错误）、`test_spec180_role_assembly.py`
（组件角色契约与真实 ONNX 子图装配）、`test_spec180_yolo_security.py`
（输入引用完整性与 terminal 单次发布）。对应已交付的共享源码；
不需要引入未提交的 SIF/Tiger 或 exporter helper。测试替身仅用于
外部依赖/注入输入，证据按 unit 或装配调用边界限定。

装配检查须显式使用本轮封存 canonical package，不能因隔离检出
缺少临时模型目录而 skip 后宣称覆盖。其余 15 个文件继续待审。
完整 C++ test-target 构建使用 46cd21a4 源码、系统 Python/Waf 与
既有本地依赖，`--targets=unit-tests,integration-tests -j2`；原始日志
保留在 `spec181-full-test-build-20260906-r1/build.log`（ignored workspace
temporary directory）。这是缺失完整构建产物的静态闭合，不是运行验收。

Batch A R1：31 passed、1 failed（1.63 s）。唯一失败在装配测试的
catalogue 签名预检，底层为隔离检出没有注册表引用的
`catalogue-authority.pub`，不是 ONNX 装配结果。日志为
`spec181-test-adoption-20260906-r1/tests.log`。测试现显式消费
`SPEC180_YOLO_CANONICAL_PACKAGE` 与 `SPEC180_YOLO_CATALOGUE_REGISTRY`
两项已有运行输入；保持真实签名验证，不复制私钥或放宽校验。R2
使用 R19 同一模型与注册表，后续交付仍须绑定注册表引用的公开材料。

Batch A R2：**32 passed，2.67 s，exit 0，无 skip**。原始日志
`spec181-test-adoption-20260906-r2/tests.log`。四个文件均使用隔离源码的
共享实现；真实 ONNX 装配检查加载并核对 certified 输入/输出名称。
测试只增加上述两项显式外部输入，未放宽签名或角色/数值检查。
该批纳入后 Python 发现文件数为 37；其余 15 个差异仍须审查。

Batch A 提交检查拒绝测试中残留的开发临时路径 fallback；提交未产生。
删除该默认路径，模型检查仅消费显式 package 输入；未配置时仅作为
普通开发测试 skip，完整 T008 必须配置且不能据此跳过。显式路径缺失
保持 FAIL。将在新 R3 重跑该批，随后按最终字节提交。

Batch A R3 最终字节重跑：**32 passed，2.78 s，exit 0，无 skip**；
日志为 `spec181-test-adoption-20260906-r3/tests.log`。主工作区与隔离
投影四文件逐字节一致；不把原提交钩子失败解释为代码或协议失败。

## Focused Configuration R1

新断言在修复前源码上运行：11 failed、4 passed、61 deselected，
1.12 s，exit 1。原始日志为 ignored workspace temporary directory
下 `spec181-local-case-config-20260906-r1/red.log`。四项声明校验未拒绝，
三份配置内容变化未改变输入身份，gate 未阻止配置漂移，三个真实
supervisor 子进程没有收到所需配置。均在命名断言处 RED，非收集或
启动环境故障。下一步按上述计划补三处生产接线并在新 R2 定向验证。

## Focused Configuration R2 And Review

实现上述 CASE_CONFIG_ENV 映射、声明校验、三配置文件身份与子进程
选取；没有修改角色数量、授权策略或 MiniNDN runner 的验收逻辑。
`/usr/bin/python3 -m pytest -q tests/python/test_spec180_inventory.py
tests/python/test_spec180_local_gate.py`：**76 passed，6.22 s，exit 0**。
原始日志保留在 ignored workspace temporary directory 下
`spec181-local-case-config-20260906-r2/green.log`；R1 RED 未覆盖。

三个实际 supervisor 子进程分别读取对应文件内容并核对 case，父环境
未被修改；修改任一配置会改变输入身份，修改 Y-B 配置使生产 gate
在启动子进程和创建输出目录前拒绝。既有 source/environment/解释器/
外部输入漂移、退出监督与清理回归同时通过。fixture 只替代被监督的
子程序，不替代此次被测 inventory/supervisor；这些检查不证明
MiniNDN 协议或模型计算。

受影响代码复审 PASS：意图对应 FR-006/008；配置选择归本地工具，
授权仍归 runner；两脚本共用映射，三条命令与 case registry 不变；
无密钥内容输出、无新远端依赖；已有 common-config 调用仍保留；
配置字节变更触发已实现输入门，定向检错证据 RED/GREEN 完整。
新身份使旧 inventory 无法直接沿用，须重新生成。完整 T008 仍受
Test Source Closure 的待处置项和完整构建清单限制，未启动完整 suite。
