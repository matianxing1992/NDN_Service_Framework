# Source Review And Runtime Validation

**Revision**: 6 | **Status**: planned; product STATIC_REVIEW NOT_RUN

本文件统一FR-018/019、SC-010/011、PO-015/016的工作流，不新增产品API。

## Workflow

各实现任务：Implementation → source/design review → necessary build → focused unit tests → implementation checkpoint。
全部实现、调用方迁移和测试/harness 编写完成后：整体接线静态检查 → final unit regression → integration tests → system/MiniNDN experiments → feature acceptance。

实现任务阶段只运行相关单元测试和必要构建/链接检查，不默认启动 integration 或 MiniNDN。
跨组件/跨进程的测试可以随实现编写、注册并静态检查，统一留到最终验证阶段运行。
按实际调用边界分类；不能把真实网络/服务协作重命名为 unit/smoke 来提前执行。
确需运行诊断才能解除具体实现阻塞时，只运行最小具名诊断并说明原因；不默认启动完整集成套件或实验，也不计为最终资格。
本Spec由T016执行一次最终完整unit suite；先前仍有效的局部单测不因下一个任务开始而自动失效。

测试可先编写；首次运行前审查受影响的生产代码、调用方和测试/collector。
一次审查同时检查：
- 设计一致性：职责、签名、调用路径、迁移和改动范围。
- 正确性：实际成功/失败路径、状态、资源寿命、并发及适用边界条件。
- 测试有效性：需求 → 生产路径 → 可观察结果 → 检错断言，防止 mock 或无条件成功造成假绿。

列出实际发现的关键风险及对应检测方法，不规定风险数量。
缺少必要检测时补验证义务；不要求每个风险新增 mutation test。
Spec 已明确的负例、反事实检错与真实运行用例必须保留。

## Decision And Retry

没有已知阻止该范围测试的明确缺陷即可 READY，并立即进入计划验证。
已知缺陷先修复，重读受影响代码；需要运行才能判断的假设进入针对性测试。
不要为风格、无证据猜测或追求完美而循环重构。
明确要求的 TDD RED / counterfactual 可运行具名受控缺陷用例，须先核对其检测路径与预期失败；
该 RED 不算产品验收，也不放行带已知缺陷的普通运行。

失败时保留原始结果，找到首个失败边界，结合日志重读相关代码，再修复和回归。
后续代码、测试、配置或依赖变化，只重审和重跑受影响范围。
全部实现完成后，同一有效审查覆盖的后续测试层级直接执行，不建立逐层放行表。
环境未启动与协议结果分开，失败/未运行均不能计为 PASS。

## One Completion Record

短记录写在 tasks.md；较长记录放已有 evidence 文件并从 tasks.md 引用：
- Task / source：任务、版本及相关未提交差异，足以定位实际受测内容。
- Changes / review：实际改动、审查范围、关键发现与处置。
- Checks / evidence：实际命令、结果、必要原始日志链接和未执行项。
- Outcome / next：实现与验收状态、剩余事项、下一步。

无需独立 S0/S1 报告、固定字段大全或每个测试层级的新记录。
提交前核对最终 diff、设计和证据是否一致，追问测试是否漏掉必需行为；
直接补充同一记录，不另设一轮全量审查。
敏感数据和大日志遵守仓库存储约定。

## Spec182 Ownership

- T001：冻结基线、具体unit/integration selectors与负例归属，关闭既有O-001--005。
- T002--T014：实现、测试编写/注册、源码审查、相关unit及必要编译/链接；集成/实验用例交给T016运行。
- T015：复用各任务已审范围，补审跨任务生产接线、测试/oracle/harness和依赖；不重复逐任务报告。
- T016：全部实现完成后，完整unit→integration→MiniNDN/no-Python及既定检错用例，核对同源证据与最终diff。
- T017：复用T016结果交接，SIF/Tiger留给实验机器。

## Acceptance

Static review PASS != Behavior PASS。

实现任务的 [x] 只表示该任务的实现、静态审查、相关单测完成；最终运行义务必须已登记到集成/实验任务，不能据此声明对应完整 PO 或整个 feature PASS。
整体验收要求静态审查符合设计、无未解决的控制性缺陷，且约定的实际构建、unit、
integration、system/MiniNDN 与必要负例全部通过，即可验收该范围。
只有新变化、失败或具体遗漏才增加受影响验证；不临时追加无关门槛。
文档检查不能证明源码逻辑或运行行为，未执行与部分通过必须如实标明。
