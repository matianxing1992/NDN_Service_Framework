# Proof Design

**Revision**: 1 | **Status**: planned; behavioral verification NOT_RUN
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
| PO-007 | FR-008; CD-001/007 | 正在执行的真实请求 cancel、deadline、late result、client close | 终态只能一次、deadline 前后事件偏序、已注册清理规则 | 不再提交新 epoch/attempt，旧结果不覆盖终态；删除 executionGuard 的有界 mutant 在语义断言失败 |
| PO-008 | FR-008/016; CD-007 | C++ Qwen cold/warm turn、续接、一次支持的 attempt replacement | 冻结完整 token 序列与 transcript，非被测 cache 命中计数 | continuation lineage 正确，旧/错 parent 拒绝，不重复输出已提交 prefix；禁用 fencing 后失败 |
| PO-009 | FR-010; CD-008 | Python 绑定和独立 C++ consumer 输入等价请求 | 固定 entropy/时钟/ACK transcript 下精确 plan bytes；实时只比较规定语义和数值 | 结果/错误/取消一致；注入 Python 策略 callback 在入口拒绝，不执行 callback |
| PO-010 | FR-011/016; CD-010 | maintained user/provider/runner 默认入口；阻断旧模块 | capability/caller inventory 与运行时 import/exec observation | 旧 coordinator/provider/helpers 被阻断仍运行；主动接回一条旧入口必须使检查失败 |
| PO-011 | FR-013/014; CD-011 | 同源完整 unit/integration + MiniNDN matrix | YOLO standalone numerical oracle、Qwen token/text oracle、进程退出与资源/secret 清理 | 所有规定 case 终端成功/注册拒绝；启动/collector 故障不得算拒绝 PASS |
| PO-012 | FR-014/015; CD-012 | 干净 checkout / handoff receiver | commit、依赖、模型/config/harness SHA-256；181 authority 前后 hashes | 本地交付身份完整，外部结果单列；替换源/依赖/配置使 gate 在启动前拒绝 |

## Negative Path Matrix

| Case / PO | First boundary | Allowed state change | Must remain unchanged | Error / cleanup / retry |
| --- | --- | --- | --- | --- |
| N01 / PO-002 | 原生策略候选 cover/预算校验 | DI 请求可进入失败终态 | 无 Selection、无 Provider load | 已有 planning error；client 清理，无自动 fallback |
| N02 / PO-003 | Core ACK_CLOSED/commit 校验 | 错误记录 | 不允许第二份冲突 plan；不可选择 ACK 集合外 Provider | Core refusal；同请求不能变更身份再重交 |
| N03 / PO-004 | Provider grant signature/recipient/expiry verifier | 请求失败/审计事件 | 不创建未授权明文/model session | DI_PROTECTED_GRANT_REJECTED；已有 lease 零化 |
| N04 / PO-005 | 原生装配路径/size/AEAD 边界 | 受控 staging 可存在直至清理 | 不激活无效 artifact/cache；不重封签名掩盖差异 | 原注册错误；清理后新 run-id |
| N05 / PO-006 | tokenizer digest/config/id 校验 | adapter 错误 | 不输出错误 text、不中途改 tokenizer | 明确 adapter error；释放原生缓冲 |
| N06 / PO-007 | cancel/deadline/late callback | 取消/截止终态与 cleanup 事件 | 不复活、不开新 attempt/epoch | Core+DI 各 owner 清理；超时等待不是自动取消 |
| N07 / PO-008 | parent/attempt/checkpoint fencing | 可以记录拒绝或不完整 staging | 既有已提交会话记录不得覆盖 | native conversation error；保留旧可恢复 checkpoint |
| N08 / PO-009 | binding native strategy type check | Python 类型错误 | callback 调用计数为零、无网络动作 | 显式 unsupported callback；不 fallback |
| N09 / PO-001/010 | runtime dependency guard | harness 记录违规并终止被测 scope | 不产出 native closure PASS | 捕获 renamed interpreter/libpython/helper service，不只检查文件名 |
| N10 / PO-011 | NFD startup / collector / Data wire-size | 原始 run/error/清理记录 | 不计注册负例通过、不混合前次结果 | 分类首边界，持久 evidence/failure index 后才重试 |
| N11 / PO-012 | 源/依赖/config/candidate 前置校验 | 只读校验记录 | 无 expensive build、网络实验或输出目录提交 | identity mismatch；先修复 source closure |

## Verification Ladder

- L0：原生公共头/库/consumer 编译链接、default import/依赖检查；不证明运行。
- L1：每单元的纯函数、编码、策略、资源/状态负例。
- L2：受影响 Core/DI/model adapter 的现有定向回归，必须登记 selectors。
- L3：真实 requester/authority/Core/Provider 生产组件跨进程协作；不可 mock 被测链。
- L4：audit PASS 后真实 MiniNDN，外部 Python harness 与被测原生运行范围分开。
- L5：所有 FR/能力清单的最终同源回归与 no-Python/legacy-exclusion。
- L6：每个 PO 至少一个错误实现或输入 counterfactual；必须在语义断言处失败。
编译失败、收集失败和环境缺失不是 L6 成功。focused L1--L3/L6 可在修复期间运行；
完整 suites/MiniNDN 正式验收必须先 T013 audit PASS。

## Planned Test and Build Inventory

下列文件/命令是 planned，当前未创建/运行；T001/O-004 将冻结旧 selectors 和新增 case 名称。
新增 tests/wscript 的注册必须与同一任务生产改动一起交付。

| Owner | Exact planned test paths | Required layers / PO |
| --- | --- | --- |
| T002/T008 | tests/unit-tests/di-native-client.t.cpp; tests/integration-tests/di-native-request.t.cpp | L0/1/2/3/6，PO-001/003/007 |
| T003 | tests/unit-tests/di-native-planning.t.cpp | L1/2/6，PO-002 |
| T004 | tests/unit-tests/di-native-plan-sealer.t.cpp | L1/2/3/6，PO-003 |
| T005 | tests/integration-tests/di-native-requester-grant.t.cpp | L1/2/3/6，PO-004 |
| T006 | tests/integration-tests/di-native-onnx-recipe.t.cpp | L1/2/3/6，PO-005 |
| T007 | tests/unit-tests/di-native-tokenizer.t.cpp | L1/2/3/6，PO-006 |
| T009 | tests/integration-tests/di-native-conversation.t.cpp | L1/2/3/6，PO-007/008 |
| T010 | tests/python/test_spec182_native_bindings.py | L1/2/3/6，PO-009 |
| T011 | tests/python/test_spec182_legacy_exclusion.py | L0/2/3/6，PO-010 |
| T012 | tests/standalone/run-spec182-native-closure.py; tests/python/test_spec182_native_closure.py | L0/3/6，PO-001/010/012 |
| T014 | Experiments/NDNSF_DI_NativeClosure_Minindn.py | L4/5，PO-001--012 中适用网络行为 |
| T001/T003--010 | tests/fixtures/spec182/case-manifest.json; tests/fixtures/spec182/native-wire-vectors.json; tests/fixtures/spec182/tokenizer-vectors.json | 冻结来源、工件摘要、单位、容差及独立 oracle |

planned command contract，cwd=repo root；--output 必须新建 run-id 目录，存在非空目录即拒绝：
- python3 tests/standalone/run-spec182-native-closure.py --manifest <case-manifest> --case <case-id> --output <new-run-dir>
- python3 Experiments/NDNSF_DI_NativeClosure_Minindn.py --manifest <case-manifest> --output <new-run-dir>
- python3 -m pytest tests/python/test_spec182_native_bindings.py tests/python/test_spec182_legacy_exclusion.py
- ./waf build --targets=ndnsf-distributed-inference,di-native-requester,di-native-provider
  （targets planned；当前 native Provider 已有，新增库/requester 由 CD-009 注册。）
C++ focused selector 由 T001 从实际 Boost/Waf target 注册中固定，不编造当前存在的测试命令。
默认每 focused unit supervisor 上限 120s；初始网络 case 上限 180s，cleanup 15s；
若既有 case 需要不同值，T001 按已有有效 deadline 固定，禁止运行中延长到 PASS。

## Runtime Without Python

运行隔离覆盖 requester、所有 Providers、进程内 authority 及其后代：
只提供原生 executable/library、模型、tokenizer、证书配置和所需 NFD/Repo 服务。
无解释器、libpython、DI Python 包；不允许 LD_PRELOAD/host venv/源码覆盖补依赖。
harness 可在隔离外用 Python 操作 MiniNDN，必须记录 PID/cgroup/mount 边界。
外部服务白名单仅为已声明 NFD/Repo/Controller；禁止隐藏 Python planner/helper service。

检查必须同时覆盖：冷缓存、未提前装配的两个 recipe、真实保护、完整文本 encode/decode，
以及动态进程执行/映射。仅 rg 找不到字符串、PATH 去掉 python、ldd 通过或暖缓存成功都不足。
O-005 冻结隔离实现后，用已知 fork-helper 版本作反例证明 gate 可发现旁路。
检查工具不能自行生成 PASS marker 后宣称被测路径纯原生。

## Acceptance Cases

- YOLO：Y-A、Y-B、Y-N-O/C/P/R/I/E/L，保持注册原因和 boundary；
  Y-N-O 为 terminal control，不要求拒绝码。
- Qwen：小型三阶段 rank-one CPU cold/warm，完整 token/text，取消/过期，
  两轮 continuation、错 parent、一次已支持的 attempt replacement。
- 其他已支持生产能力由 O-004 inventory 登记回归；无对应验收不允许声称全部迁移。
- 独立 oracle 在 runtime 隔离外预生成并封存；不得由新 C++ 被测函数生成 expected。
- 真实重复请求的 requestId/random grants 可不同，只有冻结时钟/entropy/ACK 的向量要求
  byte-identical；实时请求比较明确的语义字段和数值，不错误要求随机密文相同。

## Evidence Record

每单元 evidence/tNNN-<unit>.md 必须记录：
Task、DesignRevision、SourceIdentity、DesignClausesImplemented、FilesActuallyChanged、
SymbolsActuallyChanged、BehavioralProofs（PO/layer/oracle/result）、CommandsExecuted
（cwd/env/exit/raw path）、FailuresEncountered、DesignDeviations、RemainingRisks、
DiffScope、RecoveryState、ImplementationStatus、VerificationStatus、AcceptanceStatus、NextAction。

失败保留新 run-dir，更新 active Spec evidence/tasks 与 docs/failure-log.md，
不得覆盖历史日志或提交 secrets/大原始输出。状态为 IMPLEMENTED 不自动等于 ACCEPTED。
