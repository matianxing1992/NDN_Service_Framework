# Proof Design

**Revision**: 6 | **Status**: planned; behavioral verification NOT_RUN
本契约定义将来证明，不记录虚构测试结果。source review 不等于行为通过。

## Proof Obligations

| PO | FR / CD | Given / production entry | Independent oracle | Required result / distinguishing counterfactual |
| --- | --- | --- | --- | --- |
| PO-001 | FR-001/012; CD-001/009 | 独立安装库的 C++ consumer main 发起完整请求 | 链接依赖图、进程/文件/网络可见性检查；冷路径数值 oracle | 无解释器/DI Python 包仍成功；故意加入 exec Python 必须被 gate 拒绝 |
| PO-002 | FR-003/009; CD-002 | NativeModelSplitStrategy::enumerate + NativePlacementStrategy::propose，冻结图/ACK/时钟 | 手工小图的合法 cut/cover + 冻结策略排序向量，不用被测 sealer 生成 expected | 两策略输出可区分且合法；移除 rank/device/lease 检查后对应非法向量失败 |
| PO-003 | FR-002/004; CD-003 | NativePlanSealer → 实际 Core CommitCollaborationPlan | 已冻结 canonical JSON/签名 bytes、真实 Provider parser | 摘要和 role/tensor/namespace 一致；缺 endpoint、篡改 ACK digest、重复 commit 不得通过 |
| PO-004 | FR-005; CD-004 | C++ requester → 独立 authority → 真实 signed Data 发布/获取 → NativeGrantVerifier | Spec181 grant 固定向量与真实正向控制 | 过期/错 recipient/伪签名分别在 verifier 拒绝；密钥闲置或 grant 后授权绕过会失败 |
| PO-005 | FR-006/016; CD-005 | Native Provider 在 Selection 后调用原生 assembler | Spec181 固定装配 vectors、冻结 reference assembled bytes，不由新 assembler 自产 expected | inline/external weights、不同 recipe 字节一致；修改节点集合、路径穿越、密文/密钥错误被拒 |
| PO-006 | FR-007; CD-006 | native adapter encode/decode → 完整文本结果 | 固定 tokenizer 工件的离线 ids/UTF-8 向量，含 Unicode/special/byte fallback | digest/输出精确一致；加入 Python helper 或只返回 token IDs 必须失败 |
| PO-007 | FR-008; CD-001/007 | 正在执行的真实请求 cancel、deadline、late result、client close | 终态只能一次、deadline 前后事件偏序、已注册清理规则 | 本地不再提交新工作/attempt，旧结果不覆盖终态；已提交远端工作按已有 control/deadline 收束；删除 executionGuard 的有界 mutant 在语义断言失败 |
| PO-008 | FR-008/016; CD-007 | C++ Qwen cold/warm turn、续接、一次支持的 attempt replacement | 冻结完整 token 序列与 transcript，非被测 cache 命中计数 | continuation lineage 正确，旧/错 parent 拒绝，不重复输出已提交 prefix；禁用 fencing 后失败 |
| PO-009 | FR-010; CD-008 | Python 绑定和独立 C++ consumer 输入等价请求 | 固定 entropy/时钟/ACK transcript 下精确 plan bytes；实时只比较规定语义和数值 | 结果/错误/取消一致；注入 Python 策略 callback 在入口拒绝，不执行 callback |
| PO-010 | FR-011/016; CD-010 | maintained user/provider/runner 默认入口；阻断旧模块 | capability/caller inventory 与运行时 import/exec observation | 旧 coordinator/provider/helpers 被阻断仍运行；主动接回一条旧入口必须使检查失败 |
| PO-011 | FR-013/014; CD-011 | 同源完整 unit/integration + MiniNDN matrix | YOLO standalone numerical oracle、Qwen token/text oracle、进程退出与资源/secret 清理 | 所有规定 case 终端成功/注册拒绝；启动/collector 故障不得算拒绝 PASS |
| PO-012 | FR-014/015; CD-012 | 干净 checkout / handoff receiver | commit、依赖、模型/config/harness SHA-256；合并基线与181承接文档hashes；当前182活动指针及审查身份 | 本地交付身份完整，外部结果单列；替换源/依赖/配置使 gate 在启动前拒绝 |
| PO-013 | FR-001/002/004/009/016; CD-013 | native prepareInput/inspectModel/ensureArtifacts/verify 经 requester 生产入口 | 冻结 task encode/decode、认证 ACK/policy 和 publication 向量 | 不由 harness 预先规划；假 provenance/错 candidate/错实际名字/图身份被拒；移除任一绑定校验后反例失败 |
| PO-014 | FR-001/009/010/012; CD-014 | C++ consumer/CLI/native binding → 同一 Provider host | 真实 Core ACK/Selection/Response 与共享第二服务 | 不调用旧 Python runner；停一注册不破坏共享服务；接回旧 provider 或提前销毁 handler 会失败 |
| PO-015 | FR-018; CD-001--014 | 实现后源码/设计/测试逻辑审查，T015补整体接线 | 具体代码路径与设计条款，关键发现及处置 | 相关unit之前无已知控制性缺陷；静态结论不替代运行证明 |
| PO-016 | FR-019; CD-001--014 | T016集中运行后核对交付diff与证据 | 约定PO/用例、真实命令结果及未执行项 | 必需本地行为全部验证才整体PASS；实现任务完成不冒充最终资格 |

## Negative Path Matrix

| Case / PO | First boundary | Allowed state change | Must remain unchanged | Error / cleanup / retry |
| --- | --- | --- | --- | --- |
| N01 / PO-002 | 原生策略候选 cover/预算校验 | DI 请求可进入失败终态 | 无 Selection、无 Provider load | 已有 planning error；client 清理，无自动 fallback |
| N02 / PO-003 | Core ACK_CLOSED/commit 校验 | 错误记录 | 不允许第二份冲突 plan；不可选择 ACK 集合外 Provider | Core refusal；同请求不能变更身份再重交 |
| N03 / PO-004 | Provider grant signature/recipient/expiry verifier | 请求失败/审计事件 | 不创建未授权明文/model session | DI_PROTECTED_GRANT_REJECTED；已有 lease 零化 |
| N04 / PO-005 | 原生装配路径/size/AEAD 边界 | 受控 staging 可存在直至清理 | 不激活无效 artifact/cache；不重封签名掩盖差异 | 原注册错误；清理后新 run-id |
| N05 / PO-006 | tokenizer digest/config/id 校验 | adapter 错误 | 不输出错误 text、不中途改 tokenizer | 明确 adapter error；释放原生缓冲 |
| N06 / PO-007 | cancel/deadline/late callback | 本地终态与远端 cleanup 分开记录 | 本地不复活、不新建 attempt/Selection；已提交远端受原 deadline 约束 | 本地 cancel 不当作 remote abort；deadline+cleanupBudget 内核对清理 |
| N07 / PO-008 | parent/attempt/checkpoint fencing | 可以记录拒绝或不完整 staging | 既有已提交会话记录不得覆盖 | native conversation error；保留旧可恢复 checkpoint |
| N08 / PO-009 | binding native strategy type check | Python 类型错误 | callback 调用计数为零、无网络动作 | 显式 unsupported callback；不 fallback |
| N09 / PO-001/010 | runtime dependency guard | harness 记录违规并终止被测 scope | 不产出 native closure PASS | 捕获 renamed interpreter/libpython/helper service，不只检查文件名 |
| N10 / PO-011 | NFD startup / collector / Data wire-size | 原始 run/error/清理记录 | 不计注册负例通过、不混合前次结果 | 分类首边界，持久 evidence/failure index 后才重试 |
| N11 / PO-012 | 源/依赖/config/candidate 前置校验 | 只读校验记录 | 无 expensive build、网络实验或输出目录提交 | identity mismatch；先修复 source closure |
| N12 / PO-013 | Core provenance + DI offer policy admission | 可记录拒绝 | 不允许策略使用未认证 offer，不产生 Selection | native admission error；无 Python verifier 回调 |
| N13 / PO-013 | model/task/catalog/publication binding | 允许已经签发的不可变 Data 存在至 TTL | 不激活错名/错 digest 工件，不声称撤销已发布记录 | requester 清理临时 secrets；按原 TTL/授权失效 |
| N14 / PO-014 | provider registration/stop | 本注册关闭，既有工作按契约收束 | 不停止共享另一服务/Face，不授予管理权限 | native host error；在途 callback 安全退出 |

## Verification Ladder

执行与记录仅定义于 [validation workflow](pre-test-static-review.md)，不逐层新增报告。
- L0：必要公共头/库/consumer编译、安装链接和依赖检查，不证明完整请求。
- L1：各实现任务的编码、策略、资源/状态及适用负例unit。
- L2：受影响既有回归；unit部分随任务执行，跨组件/跨进程部分由T016运行。
- L3：全部实现后T016运行真实requester/authority/Core/Provider协作，不mock被测链。
- L4：T016在完整unit和integration通过后运行真实MiniNDN。
- L5：T016完成全部FR/能力的同源证明与no-Python/legacy-exclusion，T017交接。
- L6：保留PO-001--014及下表规定的负例/counterfactual；
  单元级随实现，涉及真实协作/隔离/实验的在T016。
  检错成功必须来自目标语义断言；编译/启动/collector失败不算。
  工作流PO-015/016无需为了报告额外制造mutant。

T002--T014不提前执行L3/L4；T015整体静态检查后，T016执行完整unit→integration→MiniNDN。
测试编写与执行分开；同一文件有不同层级时使用T001冻结的独立selector。

## Planned Test and Build Inventory

下列文件/命令是 planned，当前未创建/运行；T001/O-004 将冻结旧 selectors 和新增 case 名称。
新增 tests/wscript 的注册必须与同一任务生产改动一起交付。
下表Owner负责编写和单测；所有L3/L4及系统级L5/L6的执行owner统一为T016。
目录名不决定层级。为仅列integration文件的任务补独立unit入口，避免为通过任务而提前跑服务协作。

| Owner | Exact planned test paths | Required layers / PO |
| --- | --- | --- |
| T002 | tests/standalone/spec182-installed-consumer.cpp | PO-001 L0 安装链接已有 runtime；无 request stub |
| T010 | tests/unit-tests/di-native-client.t.cpp; tests/integration-tests/di-native-request.t.cpp | L0/1/2/3/6，PO-001/003/007/013/014 |
| T003 | tests/unit-tests/di-native-planning.t.cpp | L1/2/6，PO-002 |
| T004 | tests/unit-tests/di-native-plan-sealer.t.cpp | L1/2/3/6，PO-003 |
| T005 | tests/unit-tests/di-native-grant.t.cpp; tests/integration-tests/di-native-requester-grant.t.cpp | L1/2/3/6，PO-004 |
| T006 | tests/unit-tests/di-native-onnx-recipe.t.cpp; tests/integration-tests/di-native-onnx-recipe.t.cpp | L1/2/3/6，PO-005 |
| T007 | tests/unit-tests/di-native-tokenizer.t.cpp | L1/2/3/6，PO-006 |
| T011 | tests/unit-tests/di-native-conversation.t.cpp; tests/integration-tests/di-native-conversation.t.cpp | L1/2/3/6，PO-007/008 |
| T012 | tests/python/test_spec182_native_bindings.py | L1/2/3/6，PO-009 |
| T013 | tests/python/test_spec182_legacy_exclusion.py | L0/2/3/6，PO-010 |
| T014 | tests/standalone/run-spec182-native-closure.py; tests/python/test_spec182_native_closure.py | L0/3/6，PO-001/010/012 |
| T014 build / T016 execute | Experiments/NDNSF_DI_NativeClosure_Minindn.py | T014编写/静态审查与collector逻辑unit；T016执行L3/4/5及真实L6 |
| T001/T003--012 | tests/fixtures/spec182/case-manifest.json; tests/fixtures/spec182/native-wire-vectors.json; tests/fixtures/spec182/tokenizer-vectors.json | 冻结来源、工件摘要、单位、容差及独立 oracle |
| T008 | tests/unit-tests/di-native-preparation.t.cpp; tests/integration-tests/di-native-preparation.t.cpp; tests/unit-tests/di-native-offer-admission.t.cpp | L1/2/3/6，PO-013 |
| T009 | tests/unit-tests/di-native-provider-host.t.cpp; tests/integration-tests/di-native-provider-host.t.cpp | L0/1/2/3/6，PO-014 |

planned command contract，cwd=repo root；--output 必须新建 run-id 目录，存在非空目录即拒绝：
- python3 tests/standalone/run-spec182-native-closure.py --manifest <case-manifest> --case <case-id> --output <new-run-dir>
- python3 Experiments/NDNSF_DI_NativeClosure_Minindn.py --manifest <case-manifest> --output <new-run-dir>
- python3 -m pytest tests/python/test_spec182_native_bindings.py tests/python/test_spec182_legacy_exclusion.py
- ./waf build --targets=ndnsf-distributed-inference,di-native-requester,di-native-provider
  （targets planned；当前 native Provider 已有，新增库/requester 由 CD-009 注册。）
C++ focused selector 由 T001 从实际 Boost/Waf target 注册中固定，不编造当前存在的测试命令。
默认每 focused unit supervisor 上限 120s；初始网络 case 上限 180s，cleanup 15s；
若既有 case 需要不同值，T001 按已有有效 deadline 固定，禁止运行中延长到 PASS。

## Bounded Executor Selectors

[execution cards](spark-execution.md#verification-commands)将上表文件进一步映射到planned suite与行为卡。
T001-C冻结实际runner/build身份和选择器，所属实现卡注册后用list_content确认非空；未注册命令不算existing。
新增stream unit路径为`tests/unit-tests/distributed-inference-tokenizer.t.cpp`（T007-B）与
`tests/unit-tests/distributed-inference-stream-recovery.t.cpp`（T010-C/T011-B）；
sampler四个具名case归`tests/unit-tests/di-native-conversation.t.cpp`的`Spec182Sampling` suite。
T009-A/B/C分别覆盖Core scoped registration、共享lease与公共host，六个Registration及三个SharedLease/Closing具名case
保留在`tests/unit-tests/di-native-provider-host.t.cpp`的对应suite，不以拆卡删除旧义务。
真实worker进程cancel/timeout/crash/partial frame归T006编写、T016运行；纯协议/状态unit归T006。
本段明确执行归属，不减少任何负例或允许mock替代真实进程资格。

## Runtime Without Python

运行隔离覆盖 requester、所有 Providers、进程内 authority 及其后代：
只提供原生 executable/library、模型、tokenizer、证书配置和声明的 NFD/Repo/Controller。
白名单中的服务同样核对 native implementation/依赖身份，名字在白名单不豁免隐藏 Python 计算。
无解释器、libpython、DI Python 包；不允许 LD_PRELOAD/host venv/源码覆盖补依赖。
harness 可在隔离外用 Python 操作 MiniNDN，必须记录 PID/cgroup/mount 边界。
外部服务白名单仅为已声明 NFD/Repo/Controller；禁止隐藏 Python planner/helper service。

检查必须同时覆盖：冷缓存、未提前装配的两个 recipe、真实 raw input/图/工件准备、ACK admission、真实保护、完整文本 encode/decode，
以及动态进程执行/映射。仅 rg 找不到字符串、PATH 去掉 python、ldd 通过或暖缓存成功都不足。
T001/O-005已冻结[native isolation design](native-isolation-design.md)的文件/函数、manifest、权限/白名单、进程/映射/endpoint观测与I01--I08反例；最小工具可行性检查PASS。T014实现、T016用已知fork-helper/瞬时libpython/旁路连接等反例证明gate有效，最小工具检查不计这些反例或业务资格。
检查工具不能自行生成 PASS marker 后宣称被测路径纯原生。

## Acceptance Cases

- YOLO：Y-A、Y-B、Y-N-O/C/P/R/I/E/L，保持注册原因和 boundary；
  Y-N-O 为 terminal control，不要求拒绝码。
- Qwen：小型三阶段 rank-one CPU cold/warm，完整 token/text，取消/过期，
  两轮 continuation、错 parent、一次已支持的 attempt replacement。
- Provider host：native CLI/consumer 与 Python facade 分别验证同库 serving；callback 类型迁移、重复注册、共享服务停止安全。
- input/admission：原生完整 input/result mapping，错误 provenance/policy/catalog/publication 名称拒绝。
- 其他已支持生产能力由 O-004 inventory 登记回归；无对应验收不允许声称全部迁移。
- 独立 oracle 在 runtime 隔离外预生成并封存；不得由新 C++ 被测函数生成 expected。
- 真实重复请求的 requestId/random grants 可不同，只有冻结时钟/entropy/ACK 的向量要求
  byte-identical；实时请求比较明确的语义字段和数值，不错误要求随机密文相同。

## Evidence Record

使用 [validation workflow](pre-test-static-review.md#one-completion-record)的一份短记录，
说明实际源码/范围、审查发现、命令/结果/日志、状态和下一步。
未运行、实现/单测完成、完整PO通过分别标明；T016收齐全部既定运行证据。
失败保留新run-dir并同步tasks和docs/failure-log.md，不提交secrets或大日志。

## Symbol Documentation Proof


FR-017/SC-009：T001 对源码/设计执行双向清单核对（source symbol→contract→task→PO，以及新增契约→预期diff），覆盖重载、字段、配置、回调和关键局部状态。LOCAL_DETAIL只豁免无外部语义的循环计数等细节。T015 对实际声明、Doxygen/Python docstring和示例逐项审计；类/方法计数本身不能证明文字准确。

成功用法在独立C++ consumer与绑定测试执行；错误例核对原生原因码及Python映射，取消例区分本地终态与远端清理。删除旧默认配置项要有实际caller迁移与拒绝/弃用行为，不能只更新示例。Core请求加密/版本刷新/撤销/持久状态的既有回归在迁移后重跑；如ControllerVersion变化或撤销使在途请求失效，不允许DI缓存的过时policy或observer恢复成功。
