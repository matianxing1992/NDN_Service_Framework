# Native Projection Source Closure

**Status**: PASS (focused projection closure); T007 BLOCK
**Evidence layer**: implemented / wired / executed (focused native build and contract tests)

## Starting Boundary

承接 [framework source closure](t007-framework-source-closure-20260906.md)：
framework 单元已提交 `1df718c8`，26 cases / 204 assertions PASS。
上一轮是真实源码修复与失败定位进展；当前尚有 native source closure。

隔离检出在 tracked tree 干净时切换到 `0e59e33b`，再仅带入
`NativeExecutionPlanJson.hpp` 的两段声明：assignment-bound
`canonicalArtifactName` 与已提交 native YOLO adapter 所需的后处理
字段；没有纳入同文件的生成采样扩展。`NativeProviderHandler` 已从
认证 assignment 填充逻辑 artifact 名称，沿用该路径。

## Validation Boundary

原始日志位于 ignored workspace temporary directory 的
`spec181-native-plan-closure-20260906-r1/`。维护 native build 使用既有
host-local 配置及 `--jobs 2`，没有运行模型、正式矩阵、SIF 或 Tiger。
该检出含明确选择的修复差异，不能作为最终干净提交资格。

同时核对现有 native plan parser：工作区尚有 COMPONENT_SET、后处理
字段及同一 PIPELINE 多张量 scope 的未提交修正。后续以现有 plan/
merge 用例和新增契约边界回归验证所需差异，避免只补声明而遗漏实际
解析/传输行为。T007 保持 BLOCK，整体仍为 5/12。

## Native Build R1

仅补入上述 12 行声明后，维护 native build **PASS**：Waf 目标
5m42.562s，通过 framework/native Provider 编译链接，随后构建 Python
扩展并完成实际导入/依赖身份检查，输出 `SPEC180_NATIVE_IDENTITY_OK`，
进程退出 0。receipt SHA-256：
`f289457f122791137ace51d052517310ba34f8cf8efe48643a11e08aad78c6dd`。
原 receipt 复制为 R1 的 `native-build-receipt.json`，精确源码差异保存
为 `source.patch`；这是选择源码的本地构建证据，不是正式资格。

随后新增维护目标 `spec181-native-plan-closure`，复用既有 native
plan/merge 单元文件；加入 COMPONENT_SET 后处理解析/一致性与根
名称不能来自 JSON 的回归，并纳入已有双张量 PIPELINE scope 回归。
这些测试尚未通过，parser 修复仍待 RED 验证。Waf 源清单变化使上述
receipt 只适用于 R1 历史字节，后续 native/live 前须刷新身份。

## Focused RED R1

定向目标构建 PASS（2m31.163s），执行 **27 PASS / 2 FAIL**，
119/123 assertions PASS。首个失败是完整后处理 COMPONENT_SET 被旧
parser 拒绝；第二个失败是同一 PIPELINE 的两个不同张量都映射为
`group-0`，消费端却为 `group-0/from/S0R0`，导致 scope 重合和不一致。
JSON 根名称注入检查及其余既有 plan/merge 用例通过。

保留 R1 的 `focused-red.log`、`focused-source.patch`。修复限定于
COMPONENT_SET/后处理契约解析及多张量运行时 scope；传输授权仍绑定
原 `groupId`，生成采样扩展不纳入本单元。T007 保持 BLOCK。

## Focused GREEN R2

收口既有 parser 的 COMPONENT_SET 零层区间/节点覆盖、native
postprocess 字段读取与角色/装配一致性检查、阈值范围及排序契约。
运行时 scope 在需要区分张量时包含 producer/tensor 身份，并保留
redistribution 的既有分组；`transportScope` 仍为原授权 `groupId`。

增量目标构建 **PASS（28.075s）**；**29 cases / 133 assertions PASS**，
退出 0。包括既有 plan/merge、生成契约、保护绑定检查，以及本轮
后处理、JSON 根名称来源与双张量回归。两个新增断言明确验证内部
scope 变化不改变传输授权组。R2 的 `focused-build.log`、
`focused-green.log`、精确 `source.patch` 已保留。

选定单元为四个文件（179 additions / 10 deletions）；未纳入同文件
的生成采样扩展或 compact tensor codec 改动。可以形成源码 checkpoint；
之后须在同一提交重新运行维护 native build，才有当前身份。上述
29 项不是完整资格套件，T007 其余候选/有效配置闭包仍待审查。
