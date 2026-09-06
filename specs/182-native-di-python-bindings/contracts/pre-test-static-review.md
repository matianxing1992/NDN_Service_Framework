# Pre-Test Static Code Review Contract

**Revision**: 4 | **Status**: planned; product STATIC_REVIEW NOT_RUN
**Requirements**: FR-013,FR-018; SC-010; PO-015
**Authority**: [spec](../spec.md), [proof design](proof-design.md), [work units](work-units.md)

## SR-001 Ordering and Scope

182采用两层审查：各实施单元内S0，和T015跨单元整体审查。前者必须先于该单元的unit/integration运行；后者先于T016完整suites及MiniNDN。保留T001--T017，不按文件添加机械审查任务。

~~~text
design READY → implementation + test authoring
  → S0 read source against design → fix findings → re-read → scoped PASS
  → unit → integration
T014 harness reviewed + focused checks
  → T015 whole-path static/convergence PASS
  → T016 unit → integration → MiniNDN
~~~

每次测试启动前核对当前审查identity、AllowedTestScope和前层结果。未改动且范围已覆盖，可引用现有报告；测试从unit扩大到integration/MiniNDN时，必须审查新引入的跨组件调用、配置、fixture、launcher、collector及资源清理。T015可以预先覆盖完整范围，不要求无变化重读同一实现。

此S0比既有仅“正式资格前审计”的规则更早。现有指南中允许focused诊断先于整体PASS，不代表182可绕过该focused范围的S0。静态工具、编译、文档结构脚本只能辅助，不能代替审查者实际读逻辑。构建/依赖探针不是测试PASS；独立consumer执行也属于受门禁约束的运行检查。

## SR-002 Subject Identity

报告记录实际checkout、commit、工作区文件hash、设计版本/hash、所读路径/符号、调用图、tests/fixture/oracle/harness、有效配置与已确认依赖身份。源码尚未合并提交时保留parent和实际字节身份；HEAD相同不能证明subject相同。初始source参考[merged baseline](../evidence/merged-source-baseline-r3.json)，T001仍需刷新最终合并checkpoint。

当前182原生迁移尚未实施，因此本附件只定义审查协议，不产生任何产品STATIC_REVIEW PASS。不存在的planned类型/方法保持NOT_REVIEWED；未知关键ABI、授权或状态语义阻塞相应范围，不能由注释或旧test PASS填补。

## SR-003 Review Dimensions

| Dimension | Required inspection and reasoning |
| --- | --- |
| Design fidelity | FR/CD/INV及C/M/V契约到真实实现、未修改callers及结果/拒绝路径双向映射；识别缺失实现、额外逻辑与层次越界 |
| Control/data flow | 参数来源、类型/单位/边界、默认值、分支顺序、循环终止、返回值、规范字节/digest、异常传播；推演具体触发条件 |
| State/concurrency | 唯一owner、attempt/epoch、锁/串行executor/GIL、异步capture与对象寿命、取消/超时/晚到回调、重复commit和清理 |
| Security/authority | 认证/权限先于明文、装配和执行；grant与Core请求key分离；ControllerVersion/撤销/lease再验证、secret零化、无错误fallback |
| Configuration/wiring | 实际默认入口、注册/链接、参数优先级、配置缺失、依赖ABI、consumer运行身份；查未修改的接线处，不只看diff |
| Test/oracle/evidence | 测试经过目标生产决策；fixture不代做准备/授权；oracle独立，断言能区分目标行为；collector检查真实exit/边界，不从marker或timeout构造PASS |
| Migration/docs | 旧默认路径/动态import/callback退出；兼容签名与错误映射；注释、配置、示例与当前代码一致 |
| Counterexample reasoning | 每条关键行为有成功流及适用失败流（非法输入/拒绝、取消/超时、重复/乱序、重启/部分写入），记录具体源码分支和预期结果 |

## SR-004 Feature Review Map

ExactFiles/ExactSymbols以相关CD及[符号契约](symbol-design.md)为准，下表规定必须读透的逻辑重点；方法尚不存在时不得将此表作为源码审查记录。

| Scope | Task / CD / PO | Source reasoning focus |
| --- | --- | --- |
| Native library | T002 / CD-009 / PO-001,012 | Waf/setup导出同一库、真实消费者链接、无第二份DI实现；编译成功与完整请求分开 |
| Planning | T003 / CD-002 / PO-002 | split/placement纯提案；ACK后图检查、3字段预算、确定排序、role/rank覆盖、has_model不等于residency |
| Sealing | T004 / CD-003 / PO-003 | 单一snapshot/core、endpoint/owner/DAG校验、canonical编码、独立Provider验证及commit返回值 |
| Grant | T005 / CD-004 / PO-004 | 独立issuer、精确recipient/role/request/attempt、实际publication名/digest、权限先行与secret寿命 |
| Assembly/tokenizer | T006,T007 / CD-005,006 / PO-005,006 | 冷装配在Selection后；external-data路径/资源/取消；无helper IPC；tokenizer实例、完整文本与digest |
| Preparation/admission | T008 / CD-013 / PO-013 | raw input只编码一次、ACK provenance/policy、图/工件认证、publication与cleanup的真实接线 |
| Provider host | T009 / CD-014 / PO-014 | 复用runtime；注册token/handler寿命、重复注册/关闭和其他共享服务不受影响 |
| Request/conversation | T010,T011 / CD-001,007 / PO-007,008 | 请求和turn单一终态、单调deadline、旧attempt fencing、observer隔离、journal原子晋升 |
| Bindings/migration | T012,T013 / CD-008,010 / PO-009,010 | GIL边界与共享owner、Python无业务决策或回退；全部maintained caller和旧路径退出 |
| Harness/qualification | T014--T016 / CD-011 / PO-011,012,015 | 独立oracle、受检进程范围、no-Python有效检错、启动/collector错误首边界、审查版本与实际运行subject一致 |
| Design/delivery | T001,T017 / CD-012 / PO-012,015 | T001检查设计及合并交接，不能审不存在的迁移；T017核对报告/测试/交付身份，示例变更需要重新S0 |

## SR-005 Findings and Repair

每个finding：ID、severity、准确path:line/symbol、设计条款、输入/事件、expected/actual、因果调用链、影响、owner、建议修复、后续验证。代码推理能确定的错误与待运行验证的假设分开，不伪造复现结果。

Critical/High或任意严重度的控制性缺陷、未覆盖关键路径、未知安全/生命周期语义均BLOCK。控制性问题包括会使业务错误或测试/证据失真的缺陷。普通非控制性注释/维护项可保留但须明确owner与处置。

BLOCK → 已授权范围内修复 → 重读改动和影响面 → fresh PASS → 测试。设计本身错误先改设计/追踪，再实现；不为了贴合意外代码而倒写设计。无需增加人工确认或强制第二代理，审查者须重新阅读最终代码并提供逻辑证据。

## SR-006 Stage Entry and Invalidation

| Entry | Required static review | Other prerequisite |
| --- | --- | --- |
| Focused unit / adjacent regression | 当前单元S0 PASS覆盖生产逻辑及对应test/fixture/oracle | 设计门与构建/运行前置满足 |
| Focused integration / live consumer | S0 PASS覆盖真实跨组件调用、配置、launcher与cleanup | 适用unit先PASS；没有独立unit适用范围时明确N/A理由 |
| Full unit suite in T016 | T015整体PASS及suite范围identity检查 | T014完成；全部相关单元证据齐全 |
| Full integration in T016 | 相同subject有效T015 PASS，覆盖全部接线/harness | 完整unit PASS |
| MiniNDN in T016 | 有效T015 PASS覆盖topology/collector/oracle/退出与清理 | 完整unit和integration PASS |
| Retry after source/test/config repair | 受影响审查STALE，先修复并重新S0 | 保存本次raw/首边界/evidence，按依赖重跑失效层级 |

source/design/test/oracle/config/dependency/harness的行为相关变化使受影响审查STALE，并使其下游测试证据回到待复核。纯文字修正经影响分析记录可保留原运行证据；不得自动重复全套实验。测试失败不能被“审查已PASS”否定；应记录静态审查遗漏并修复，后续结果仍独立判定。

## SR-007 Named RED and Counterfactual

显式TDD或PO规定的mutant运行前也要S0：审查修复前/变异代码、具名expected defect、测试断言、隔离和预期语义失败。该报告的AllowedTestScope只能是具名RED/变异case；结论仅为受控检错检查可执行，不证明产品符合设计，也不放行普通测试。未知缺陷不能重新命名expected defect绕过门禁。恢复正常代码后重审才能GREEN；保留原始RED证据，不把编译/启动失败当mutant被检出。

## SR-008 Evidence and Verdict

计划路径：每单元 `evidence/tNNN-static-review-rN.md`；整体 `evidence/t015-static-review-rN.md`。
报告文件当前未创建，不虚构完成。报告至少包含：

~~~yaml
ReviewId: <task-scope-revision>
Mode: pre-test-static
Task: <TNNN>
DesignIdentity: <revision-and-hashes>
SourceIdentity: <commit-and-worktree-hashes>
TestSubject: <selectors-fixtures-oracles-config-dependencies-harness>
ReviewedPathsAndSymbols: []
DesignCodeMapping: []
PathWalkthroughs: []
Findings: []
ResolvedFindings: []
RemainingFindings: []
ExpectedDefect: null
AllowedTestScope: []
Verdict: NOT_REVIEWED-or-BLOCK-or-PASS-or-STALE
Limitations: []
Supersedes: null
InvalidationAndNextAction: []
RuntimeTests: NOT_RUN
~~~

NOT_REVIEWED/BLOCK/STALE不能放行；PASS要求当前范围覆盖完整、控制性finding清零且记录明确。CONDITIONAL PASS不作为测试入口。每次运行evidence记录ReviewId、源身份核对、allowed scope和前层结果。报告中的RuntimeTests=NOT_RUN描述本次静态审查，不抹除独立记录的既有测试历史。

## SR-009 Proof and Limits

PO-015检查两类证据：审查者逐源码路径的真实逻辑对照；测试启动记录引用有效报告并遵守阶段依赖。若缺报告、审查BLOCK/STALE、hash漂移或测试范围超出，工作流必须在运行前停止。此阶段由实施代理执行规则核对，不凭空声称已经有自动阻断器或现成CLI。

结构脚本只核对门禁条目/链接/ID；它无法证明审查者读过代码、结论正确或所有bug已找到。运行时并发、数值、网络与环境行为仍必须通过unit/integration/MiniNDN确认。当前附件审查协议的文档PASS不等于未来产品STATIC_REVIEW PASS。
