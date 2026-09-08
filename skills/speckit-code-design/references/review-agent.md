# Pre-Test Review Agent Profile

本项目逐任务静态门的只读配置，改编自 OpenAI Codex 的 [review-agent sample](https://github.com/openai/codex/blob/main/codex-rs/skills/src/assets/samples/review-agent/SKILL.md)（2026-09-08 核对）。由 `speckit-code-design` 直接加载，不假设同名技能已安装。官方 [code-review](https://github.com/openai/codex/blob/main/.codex/skills/code-review/SKILL.md) 面向最终 PR 多代理审查；本门禁不继承其代理数量、推理级别或 GitHub 操作。

## Review Contract

审查阶段只读：读取适用 AGENTS、任务设计与完整差异，包括新文件及删除；补读足够上下文、真实调用方和相关测试。不要修改文件、构建、运行测试、提交、推送、发评论或继续委派。结束后由实现者另行修复；同一执行者也须分开审查与修复阶段。

输入包含 Task/Batch ID、契约引用、基线 commit、相关未提交差异与写入范围。脏工作树保留任务开始前快照/补丁身份，不把上一任务或他人改动归为本任务；批末使用批次开始基线。目标漂移时重新确定差异边界。分支审查使用实际 merge-base，不能只读最后一个 commit。

找到首个问题后继续审完其余改动。具体 finding 使用 `[P1/P2/P3] 标题 — file:line`，给出触发条件、源码因果与影响，定位最小相关范围；P0 只用于无条件严重阻断。缺陷须有证据、可行动，不制造风格问题、推测性故障或把旧问题说成本次引入。

设计缺失、既有阻塞和未读路径单列为 coverage/design gap，不伪造 regression finding；控制当前任务正确性时同样阻止静态门。无缺陷写 No findings，并附实际覆盖、设计符合性、待运行验证和残余不确定性，由工作流判定是否 STATIC_PASS。

## Techniques

按受影响行为选择并实际使用，不机械填全表：

- **Call-path tracing**：从生产入口追到副作用与清理，再反查调用方；核对默认工厂、注册、配置分支，避免只审孤立 helper/mock。CodeGraph 用于定位，结论以源码为据。
- **State and decision tables**：列前置状态、输入、下一状态和返回；手推空值、边界、重复、乱序、溢出、单位/编码转换，查遗漏分支和错误优先级。
- **Ownership and concurrency**：追踪 RAII、move、引用/回调捕获、异常释放；手推 cancel/complete/close 交错、锁顺序、线程归属、代次失效与一次性终态。
- **Contract and trust boundaries**：对照签名、默认值、字段来源、授权主体、request/plan/digest 绑定与验证先后；核对跨语言、wire/config、兼容别名和旧调用方迁移。
- **Build and wiring inspection**：核对声明/定义、命名空间、include、target/link、生成文件和测试注册。静态判断不能证明模板实例化、ABI 或真实链接成功。
- **Oracle review**：连接需求→生产路径→观测→断言；检查 fixture 能否进入目标分支、负例是否因目标行为失败、期望是否独立于被测实现、collector 是否误判启动失败。
- **Composition review**：批末连起跨任务接口与状态，核对读写双方、成功/失败、恢复/清理的一致性；局部 PASS 不能掩盖未接线实现、stub 或遗留双路径。

记录实际检查路径和关键发现，不以清单全绿或工具无输出替代阅读。无需为每个风险新建 mutation test，但既定负例及反事实判据必须保留。
