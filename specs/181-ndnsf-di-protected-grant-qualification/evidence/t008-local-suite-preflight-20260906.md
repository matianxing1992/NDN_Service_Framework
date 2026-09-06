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

## Full Test Build R1 And Runtime Test Migration

以下运行记录之外的剩余测试来源处置见本文末尾 Remaining Test Source
Disposition；登记的 local-suite 选择器规则保持不变。

完整 test-target 构建在 `distributed-inference-protected-runtime.t.cpp`
的旧 `revoke/revoked` 调用处 exit 1。首边界为编译，未执行 unit 或
integration。该接口已按用户决定移交另一分支，不恢复接口来迁就
旧测试。主工作区的未提交版本也仍含 “until T002” 历史注释。

迁移设计：旧文件保留缺少 grant config、仅 binding 一致不授予权限、
绑定替换和过期拒绝；改为当前语义命名。其原有数据流/零化证明迁入
`distributed-inference-protected-runtime-grant.t.cpp` 的真实 BoundGrantFixture，
先精确获取并验证固定 grant，再断言 publish/fetch endpoint、local/peer
角色边界和取消/拒绝后 host/device buffer 清零。已有真实 fixture 的
cleanup-failure/retry 检查保留。增加 Waf focused target
`spec181-protected-runtime-closure`，复用现有 DI sources 与本地库，执行
上述两文件；通过后继续完整编译。不以编译错误冒充语义 RED。

恢复中断补丁时发现 `tests/wscript` 重复 `name='unit-tests'`；已移除
该重复参数，并添加上述独立 target。两份测试在主工作区与隔离检出
逐字节一致；主工作区其他任务新增的 tokenizer source 不纳入本单元。
定向构建原始目录为 `spec181-protected-runtime-closure-20260906-r1/`，
使用系统 Python/Waf、既有 `build-system-j2` 配置与 `-j2`。

R1 构建 exit 0（2m4.495s），两文件 **30/30 用例、225/225 断言 PASS**；
数据流与取消/拒绝清理新增用例独立 63/63 断言。复审将该用例 buffer
声明提前到 runtime 之前，确保异常离开时清理回调不访问已析构 buffer；
不改变生产实现或断言。最终字节在新 R2 构建/重跑，保留 R1。

R2 最终构建 exit 0（14.274 s），测试 exit 0，**30/30 用例、225/225
断言 PASS，无 skip**。从隔离仓库根执行：

```sh
env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS \
  PATH=/usr/bin:/bin:/usr/local/bin /usr/bin/python3 ./waf build \
  --out=build-system-j2 --targets=spec181-protected-runtime-closure -j2
env PATH=/usr/bin:/bin:/usr/local/bin \
  ./build-system-j2/spec181-protected-runtime-closure \
  --report_level=detailed --log_level=test_suite
```

原始目录为 ignored workspace temporary directory 下
`spec181-protected-runtime-closure-20260906-r2/`。构建为 `46cd21a4` 加
本单元两测试及 Waf target 投影；主工作区两测试字节与已测投影一致。
生产源码未改；ldd 确认使用隔离 Core、系统 ndn-cxx 0.9.0、既定 SVS
build library 和 `/opt/onnxruntime`，无缺失动态库。Core SHA-256 仍为
`0e748103217e3f5af7038cc15d8aee0863e3c832a954ba9b7338df2d1d7d5370`。

| Artifact | SHA-256 |
| --- | --- |
| R2 build.log | `4fd6603d398711b1a17d73c06049e650a37e9c0f085028c6873f9ee363e423fa` |
| R2 tests.log | `2e4918617bfa7046668caef9f67d7f4eb5431347b7c03e8dc197f76448c35555` |
| focused executable | `35261c947f537d5cef7dc4910e87e12a03883a34f17a8d37aa12fb79001de1e5` |
| protected-runtime.t.cpp | `403c93e6865fc84aca28ed5ead2de9afa81acfb1ebe187f44b8b1641fb69dcb4` |
| protected-runtime-grant.t.cpp | `f4c2a982a50170bd2a363aaf6c2c7cad5874055415abb88b0c68d7145855a906` |

迁移复审 PASS：缺配置不授予权限；结构一致不冒充 grant 验证；两种
数据流各自的 endpoint/local/peer 边界触发真实 runtime 拒绝、内容
密钥不可用和 host/device lease 清理；取消正向也清理。device lease
以测试 buffer 验证回调契约，不是 CUDA 设备内存实验。fetch callback
返回固定签名向量、密码学校验使用生产 verifier；本单元不证明 NDN
获取链。已有 cleanup-failure/retry、worker guard 等 29 个用例保留。
该定向修复关闭旧测试接口边界；完整 C++ 构建、剩余 15 份测试处置及
正式 T008 验收仍待完成，不能把此 PASS 升级为完整 suite PASS。

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

## Remaining Test Source Disposition

以 `4bd1998b` 提交与主工作区预存未跟踪草稿作比较，余下 15 文件
初审分为七项本地待纳入和八项保留移交/历史工具草稿；Batch B 后
本地两项已验证纳入，尚余五项。此表不是删除测试
或选择器豁免：已提交的继承测试仍由现有发现规则全量收集；本地
待纳入项须先审查依赖、修正过期 fixture，再在隔离源码验证后提交。
未交付草稿留在原工作区，T009 必须把它们列入未纳入交付的说明。

| Test file (tests/python/) | Disposition / owner | Evidence and required next action |
| --- | --- | --- |
| test_spec180_candidate.py | LOCAL_PENDING / T009 | 旧 helper 强制 SIF 等十平面；按已批准本地交付契约修订 helper 与测试，封印发生在 T008 后。 |
| test_spec180_native_evidence.py | LOCAL_PENDING / T008 | 实际编译 ExecutionEvidence / CudaDeviceIdentity，模拟 CUDA 边界；保留证据诚实性回归，纳入两份 C++ fixture，修正已抽取公共准备 owner 的旧源码断言。不能视为 GPU 资格。 |
| test_spec180_yolo_adapter.py | LOCAL_PENDING / T008 | 真实 catalogue/候选/分区检查依赖 exporter；纳入并验证本地工具与显式 checkpoint 输入。 |
| test_spec180_yolo_application.py | ADOPTED / T008 | Batch B R2 PASS；生产入口、输入和终端 owner，源码断言与实际函数检查分别限定证据范围。 |
| test_spec180_yolo_equivalence.py | ADOPTED / T008 | Batch B R2 PASS；共享投影/原生 Merge/canonical 发布，显式 package/registry、真实签名与当前 adapter 路径。 |
| test_spec180_yolo_export.py | LOCAL_PENDING / T008 | pinned checkpoint、签名、实际 ONNX 导出；审查 exporter 和外部模型输入闭包。 |
| test_spec180_yolo_numerical.py | LOCAL_PENDING / T008 | 数值 oracle、独立预处理及真实 User 函数；补齐 exporter 依赖及当前 tensor helper 接线。 |
| test_spec180_contract_gate.py | TRANSFERRED_DRAFT / experiment tools owner | 目标固定 Spec180 文档与九个 SC，运行调用方为 spec180_release.py；不能将旧文档门冒充 Spec181 审计。本地 catalogue 验证由已提交 adapter 与本地签名回归覆盖。 |
| test_spec180_dispatcher.py | TRANSFERRED_DRAFT / experiment tools owner | run_spec180_case.py 的 yolo-functional / QWEN-F workload，调用方为 SIF/release；本地维护 runner 由 inventory 直接注册。 |
| test_spec180_qwen_entrypoint.py | PRESERVED_DRAFT / local development owner | QWEN-F 模型 manifest 与 delegate，不是本 Spec 的 Y-A/Y-B/Y-N 或共享 runtime 验收；后续适用开发范围再纳入。 |
| test_spec180_qwen_reference.py | PRESERVED_DRAFT / local development owner | 冻结 Spec175 tiny Q-C/Q-W 参考与 Tiger 27B 区分；保留历史，不能据此声称当前 Qwen 已验收。 |
| test_spec180_release_workflow.py | TRANSFERRED_DRAFT / experiment tools owner | spec180_release.py、Slurm profile/dispatch/remote mount 身份；T010/T011 移交范围。 |
| test_spec180_terminal_collector.py | TRANSFERRED_DRAFT / experiment tools owner | packaging jobs/spec180 collector 与 validate_spec180_results.py 的 profile 结果；本地维护 collector/negative oracle 由已提交测试覆盖。 |
| test_spec180_tiger_contract.py | TRANSFERRED_DRAFT / experiment tools owner | 单 GPU 三 CUDA 角色及原生 CPU Merge/Tiger args，属于 T011。 |
| test_spec180_tiger_supervision.py | TRANSFERRED_DRAFT / experiment tools owner | supervise-tiger.py、SIF source builder 与远端 validator；本地 supervisor 自身 76 项定向回归已有记录。 |

依据：CodeGraph 定位工具后，对 `scripts/`、`Experiments/`、`packaging/`
和 DI 源码核对真实引用。contract gate 的运行调用方为 release；dispatcher
由 release/SIF build 使用；远端 validator 由 Tiger supervisor/collector
使用。未触发远端脚本、SIF 构建、网络任务或 Git 合并。表中待纳入项
仍为 BLOCK 的实际工作，不将静态分类记为回归 PASS。

## Full Test Build R2

旧测试迁移 checkpoint 为 `4bd1998b`。七个投影文件与该提交逐字节
核对后，隔离 checkout 已切到此提交，只有构建目录为未跟踪产物。
同一 `--targets=unit-tests,integration-tests -j2` 静态构建已启动，原始
日志为 `spec181-full-test-build-20260906-r2/build.log`，结果待收集。

## Test Adoption Batch B

本批纳入 application 与 equivalence 两份预存本地回归。先把等价检查
的隐藏临时 package 路径改为显式 `SPEC180_YOLO_CANONICAL_PACKAGE`，
缺配置仅作普通开发 skip、显式缺失文件 FAIL；正式 gate 必须配置。
原生 Merge 源码检查跟随实际 owner `cpp/adapters/yolo/`。其余断言
保持后，对 `4bd1998b` 加两测试投影执行定向 pytest。

R1：**18 passed、1 failed，3.01 s，无 skip，exit 1**。唯一失败是
application 的源码文本断言仍要求硬编码 `data_v1_no_progress_ms=10_000`，
需要核对当前超时参数来源再修正过期断言；不是一次网络超时。
日志为 ignored workspace temporary directory 下
`spec181-test-adoption-b-20260906-r1/tests.log`。实际 Controller publication
函数及 APPProvider 入口检查通过，不能推广为真实 NDN 运行通过。

R2：**19 passed，2.88 s，无 skip，exit 0**。实际 User 采用
`data_v1_no_progress_ms=int(args.timeout_ms)`，源码检查已跟随当前请求
预算契约，不修改超时行为。等价测试改为 `registry_path` 调用实际
签名验证，不再采用 `require_signature=False`。两测试文件与隔离已测
内容逐字节一致，源基线仍为 `4bd1998b`，无生产修改。

```sh
env PATH=/usr/bin:/bin:/usr/local/bin \
  PYTHONPATH="$PWD/pythonWrapper:$PWD/NDNSF-DistributedInference:$PWD/NDNSF-DistributedRepo/pythonWrapper" \
  SPEC180_YOLO_CANONICAL_PACKAGE="$CANONICAL_PACKAGE" \
  SPEC180_YOLO_CATALOGUE_REGISTRY="$CATALOGUE_REGISTRY" \
  /usr/bin/python3 -m pytest -q \
  tests/python/test_spec180_yolo_application.py \
  tests/python/test_spec180_yolo_equivalence.py
```

`CANONICAL_PACKAGE` / `CATALOGUE_REGISTRY` 为 R19 与 Batch A 已声明的
真实外部输入，运行目录为隔离仓库根。原始 R2 目录为
`spec181-test-adoption-b-20260906-r2/`；tests.log SHA-256 为
`394fa0b30b8cb9df80d130eb93780aa9b8c0f3c7250cd370e5ba9855d3285601`。
application / equivalence 最终测试源码 SHA-256 分别为
`a22610d5c7e5ee80172170be36a815bda6bde52b70e0b55e1f26b96f5abec071` /
`6d4fe3be20c19ce281f161add3bd5093e65c67db8c7a9cba8706409eedf9b5c6`。

复审 PASS：15 个 application 检查含源码接线约束和实际签名密钥重载、
Controller decoder/publication、APPProvider facade 调用；外部 ServiceUser
为替身，只证明 publication 生命期意图和调用，不证明网络传输。四个
equivalence 检查覆盖实际 catalogue/共享候选投影、Merge 无 ONNX 装配、
canonical transport 与身份分离及 adapter 依赖约束。没有放宽验收或
修改业务 deadline。当前提交准备纳入的 Python 发现文件数变为 39；
余下五项本地依赖和八项保留草稿如表所列。完整 T008 仍未验收。
