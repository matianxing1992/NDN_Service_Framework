# Framework Source Closure

**Status**: PASS (focused framework closure); T007 BLOCK
**Evidence layer**: implemented / wired / executed (focused build and contract tests)

## Starting Boundary

承接 [committed native closure](t007-committed-native-build-20260905.md)
R4：干净 `6c7a0b23` 已越过 Waf 建图，但 `ServiceUser.cpp` 编译
因缺失配套声明失败；上一单元属于实际进展，未重复启动原失败进程。

从 `6f2e4ac4` 建立隔离检出 `spec181-source-closure-20260906-r1/`，
路径位于 ignored workspace temporary directory。仅带入当前编译错误
对应的六个文件变更（145 additions / 3 deletions），保留其他预存
修改在主工作区；`ServiceUser.hpp` 的日志开关变更未纳入。

## Selected Dependency Map

| Source | Required behavior |
|---|---|
| `ndn-service-framework/ServiceUser.hpp` | 已提交 ACK 验证来源、发布结果与回调签名的声明；SelectedParticipant canonical root 引用 |
| `ndn-service-framework/InvocationStream.hpp` | 已提交请求取消实现所需的同步 cancelFence 及公开 handle 调用顺序 |
| `ndn-service-framework/NDNSFMessages.hpp/.cpp` | assignment envelope 的 canonical root Data 名称字段与有界编解码 |
| `ndn-service-framework/utils.hpp/.cpp` | 已提交 User/Controller 调用所需的有限注册重试声明与实现 |

这些机制属于 Core 的服务无关生命周期、认证来源和不透明 assignment
元数据；没有把 YOLO/Qwen 模型语义放入 Core。CodeGraph 核对后以当前
文件和隔离 diff 为准，索引中的旧临时树不作为提交源证明。

## Validation Plan

使用既有配置参数构建 `ndn-service-framework` 目标，先验证 framework
编译/链接依赖。随后以既有生命周期与 opaque assignment 用例作定向
回归；通过后形成精确源码 checkpoint，再验证维护 native build 的
下一层。此检出含明确选择的源修改，属于修复诊断，不是干净提交资格。
原始配置/编译日志保留在 `spec181-source-closure-audit-20260906-r1/`。
T007 与后续正式资格门仍未通过。

## Framework Compile Result

隔离 framework 编译/链接 **PASS（3m28.249s，退出 0）**，解决 R4
全部 ServiceUser 编译声明缺口。新增维护 Waf 目标
`spec181-framework-closure` 直接链接该生产库，复用既有生命周期与
opaque assignment 测试文件，避免主工作区旧二进制掩盖未提交依赖。
定向执行结果待记录，不把 framework 编译当作完整 native build。

## Focused RED R1

定向目标构建 PASS（19.808s）；执行 15 个既有生命周期用例及两个
assignment 用例，**16 PASS / 1 FAIL，145/146 assertions PASS**。
唯一失败位于 `ServiceProviderPreservesStructuredAssignmentSetForCollaborationHandler`：
Provider 处理结构化 assignment 集合后，handler 看不到预期的
`artifactDataName`。生命周期和 envelope 编解码均已通过。

R1 的 `focused-red.log` 与精确隔离 `source.patch` 已保留。首次行为
失败归属 Core 的 envelope → CollaborationContext 元数据传递，尚未
进入 DI 装配或真实网络。下一步仅带入已审阅的 Provider 字段传递与
冲突根拒绝逻辑，再在新 R2 目录复验。

## Focused GREEN R2

仅补入 `ServiceProvider.cpp` 的两段 assignment 元数据传递：单个
envelope 保留 `artifactDataName`，集合合并保留一致根并拒绝冲突根。
未带入该文件其余预存变更。增量目标构建 **PASS（1m21.561s）**。
同一维护目标的全部既有用例 **26 cases / 204 assertions PASS**，
退出 0，包含 15 个生命周期用例和 11 个 opaque selection 用例。
`ldd` 确認目标使用该隔离检出下的 framework shared library。

最终选定源码为九个文件（176 additions / 3 deletions），包含复用
测试的 Waf 目标与七行 assignment 根传递断言；精确 R2 `source.patch`、
`focused-build.log`、`focused-green.log` 已保留。本单元证明 framework
源码闭包与上述定向契约，不证明完整 native Provider、Python 扩展、
MiniNDN、SIF 或 Tiger 资格。后续从同一源码提交运行维护 native build。
