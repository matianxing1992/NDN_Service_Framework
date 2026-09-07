# Code Design Contract Template

本参考规定信息完整性，可按现有 Spec 结构合并。重复结构用同一权威表和稳定 ID 引用；不要在三个文档维护相同签名。以下为写作说明，生成正式规格时应替换成项目的具体事实与决定。

任务仅填写 [work-unit-contract.md](work-unit-contract.md) 的成果、范围、特有约束和验证分工；共享设计与执行规则通过链接引用。无需逐任务复制模板。

## 1. Design Baseline

记录仓库、branch、commit、影响设计的未提交改动、源码/架构来源与验证日期。声明 existing 的含义；planned 新符号与工作区已有实现分开。

列出需求和现状之间的具体缺口、最终可观察结果、范围外事项及本轮交付环境。依赖其他分支的改动时明确前置条件，不能假定已经合并。

## 2. Decision Register

| Design ID | FR / SC | Current behavior | Intended behavior | Decision and rationale | Alternatives / tradeoffs | Status |
| --- | --- | --- | --- | --- | --- | --- |

为新增方案、修改方案及退出的旧方案分别说明效果。性能、可用性或安全方面的收益必须有机制解释；尚未测量的收益标为预期，不写成结果。

## 3. File And Symbol Change Manifest

逐符号细节必须满足 [symbol-contract.md](symbol-contract.md) 的 Class / Function / Field / Documentation Contract，并有 coverage ledger。下列表格只作导航，不能替代详细说明。

| CD | Path | Layer / owner | Operation | Class / module | Functions / fields | Intended delta | Callers / dependents | Verification |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |

Operation 使用 ADD / MODIFY / DELETE / MOVE / REUSE。REUSE 表示经核实可原样依赖，不冒充代码改动。路径必须具体；同名重载、声明与实现、生成绑定的源文件分别可定位。新类写明职责、依赖、构造/析构约束；删除类列出所有消费者的迁移去向。

清单覆盖生产代码、头文件/导出、绑定、构建注册、包内资源、配置、测试、迁移和维护文档。MOVE 必须有源和目标，以及 import/include/安装路径更新。生成文件记录生成源和命令，不建议直接修改生成结果。

## 4. Function Contracts

每个受影响函数使用一个条目，可将完全相同的契约合并并明确列出覆盖符号。

- **Identity**：CD、文件、类/namespace、方法名及重载。
- **Operation**：新增/修改/删除/移动/原样复用。
- **Purpose**：为何存在、负责的领域动作、对应的可观察结果。
- **Before / After signature**：准确参数名、类型、顺序、默认值、返回类型、可见性，以及适用的 const/static/async/异常约定。不存在的前态写 absent。
- **Input contract**：使用下面的参数表。
- **Output contract**：返回值各分支、空值语义、单位/编码、状态改变、错误类型与调用方处置。
- **Behavior**：前置条件、关键步骤、后置条件、不变量；复杂算法提供必要伪代码，尤其分支选择与状态提交点。
- **Effects**：I/O、缓存、持久化、事件、回调、资源分配与释放；无副作用则明确。
- **Call sites**：现有调用方及修改方式、新调用方、被调方法；不可调用的层次和对应架构理由。
- **Compatibility**：旧签名的迁移/适配/删除及验证方式。

| Parameter | Type / shape / unit / encoding | Meaning and necessity | Source / producer | Valid range / absence / default | Ownership / mutability / lifetime | Validation / rejection |
| --- | --- | --- | --- | --- | --- | --- |

Parameter Review 必须回答：

1. 是否可从权威上下文推导，若仍传入，理由是什么？
2. 是否与另一参数重复或允许互相矛盾？用什么不变量拒绝矛盾？
3. bool 是否隐藏模式？字符串是否混淆不同 ID、路径、摘要或时间单位？是否需要具名类型或明确枚举？
4. Optional/default 是否有业务语义，还是悄悄掩盖调用错误？
5. 是否为减少参数而引入无边界 context/dict？复合类型应有内聚职责，不能让依赖藏进对象。
6. 异步调用结束前对象是否仍有效？谁负责释放、取消和线程切换？

仅对适用项做具体判断，不因为清单存在而强加类型系统或过度包装。

## 5. State And Data Contracts

逐项列出受影响的成员变量、类变量、共享/模块状态、配置项和序列化字段。

| CD / owner | Field and operation | Authoritative / derived and source | Type / unit | Meaning and necessity | Initial value / creation | Writer / reader | Update / reset / destruction | Invariant / synchronization | Persistence / wire effect |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |

关键约束：

- 区分事实与派生缓存；派生值写明失效条件，不新增第二个事实来源。
- 对状态机列出状态、事件、guard、action、下一状态、非法事件及终态。
- 对异步或共享状态说明锁/事件循环归属、回调晚到、超时、取消、重复事件、资源回收。
- 对长度、摘要、ID 和签名等跨层字段明确字节来源、规范化、编码、校验位置；不得只写“两端一致”。
- 对持久化/协议变更说明版本、兼容行为、部署顺序、旧数据迁移及回退边界。

## 6. Call Flow And Data Flow

给每条主流程稳定 FLOW ID。使用调用表，复杂流程可补 Mermaid sequenceDiagram；图中的符号必须存在于清单或核实过的现有接口。

| Step | Caller → callee | Actual arguments and source | Transformation / validation | Return / event | State change | Failure / cleanup owner |
| --- | --- | --- | --- | --- | --- | --- |

至少覆盖正常流程和会改变设计的失败分支；按适用性补重复/乱序、取消、恢复与部分成功。入口包括真实 CLI/服务/框架回调，出口包括用户可见结果或持久状态，不能仅画内部 helper 之间的调用。

检查每个实参与形参一致；字段在首次使用前由明确步骤产生；每个新增方法至少有可达调用方。新旧调用路径并存时说明路由和退出条件。

## 7. Reuse And Variation

| Capability | Existing implementation / consumers | Shared invariant | Actual variation | Shared API / owner | Adapter input / hook | Migration and regression coverage |
| --- | --- | --- | --- | --- | --- | --- |

以数据语义、控制流和生命周期判断复用。比如两个推理案例共享部署/调度机制时，逐项列出共享部分，模型特有的输入、输出及执行策略由适配器提供。明确共用类调用适配器的接口和实例来源，不能只写“抽象公共逻辑”。

若现有案例迁移成本超过收益，可以限定本轮迁移范围，但必须说明保留重复的原因及结果，不能声称已经统一。通用层不得反向 import 案例实现。

## 8. Build, Runtime And Delivery Closure

设计阶段列出：新增源码如何编译/安装/导出，运行入口如何发现它，资源及配置如何取得，外部依赖如何限定，验证从哪个基线运行。新文件仅存在于工作区或开发机缓存不能构成可交付依赖。

| Artifact / path | Producer / source | Consumer / environment | Required version / identity | Planned creation or change | Verification |
| --- | --- | --- | --- | --- | --- |

开发验证与后续实验的责任、交付 commit、环境信息、复现步骤、结果反馈字段分别定义。交接项标记 TRANSFERRED/DEFERRED 时保留负责人和接受条件，不能勾选为实验 PASS。只为当前需求规定必要环境，不擅自扩大到远端部署。

## 9. Traceability And Acceptance

源码审查、运行验证和完成记录统一遵循 [pre-test-static-review.md](pre-test-static-review.md)，本节只定义具体行为与判据。



| FR | SC / observable outcome | INV / CD / FLOW | Task / work unit | PO | Exact artifacts / symbols | Validation layer and oracle | Evidence destination |
| --- | --- | --- | --- | --- | --- | --- | --- |

每条验收说明输入、动作、预期值/状态、容差（适用时）、失败判据和依据来源。依据应来自需求、协议或独立参考，不能来自待验证函数自身的输出。

不仅验收“新函数存在”，还应证明生产路径使用它、旧路径按计划退出、共享消费者没有回归，及实际交付包含所需资源。测试命令须已核实存在，或明确是本轮将新增的工具和任务，不能编造可运行命令。

任务中写明前置条件和结果，而非“修复必要问题”“处理其他细节”。设计阶段可以把测试方法标成 planned，不得伪造已执行证据。


## 10. Open Questions And Change Control

| Open ID | Unknown | Evidence needed / bounded investigation | Designs affected | Resolution / evidence | Status |
| --- | --- | --- | --- | --- | --- |

只对确实不影响本轮成果的项允许带理由延期。其余在相关实现前解决；无法解决时报告设计边界和缺少的事实。

设计修订记录旧决定、新决定、证据、影响到的文件/调用方/任务，以及需要重跑的检查。明确区分：设计修订完成、实现完成、验证完成、资格认定完成。

沿调用与依赖关系列出受影响的 PO、此前证据是否失效及理由、最早重跑层级与任务状态调整，在同一任务结果记录中说明。设计修订不能只更新签名而继续引用已不适用的 PASS。

## 11. Design Readiness

记录状态 DRAFT / BLOCK / READY_FOR_IMPLEMENTATION、评审 baseline、覆盖范围、发现及处置、剩余限制。按照 review-gate 检查后才可声明就绪。若只批准了某个可独立范围，明确列出 CD/FR，不将部分就绪推广为整个 Spec 就绪。
