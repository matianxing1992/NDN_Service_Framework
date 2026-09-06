# Exact Data Wire Repair

**Evidence layer**: implemented / wired / executed (focused Core repair only)
**Status**: IN_PROGRESS (Core focused PASS; DI transport BLOCK)

## Baseline And Controlling Failure

基线 `b95b7e84`；R18 实际源码 `c0d585f1`。MANIFEST 与 SEG 的完整
签名 Data 超过 8,800 bytes，首次发送失败，后续依赖超时。详见
[R18](t005-formal-matrix-20260906.md#exact-dependency-boundary-r18)。
本记录属于 T005/A05 定向修复，不证明正式矩阵或 T008 资格。

## Repair Boundaries

Core 仅负责所有精确 Data 的完整签名 wire 大小检查：
`ServiceProvider::publishCollaborationSignedExactData` 先构造并签名整个
批次，任一对象超限就返回 false，全部大小检查通过后才插入 IMS。
不得以 content 长度代替 wire 长度；不改变包大小上限。该行为可以
独立验证和提交，但单独完成它不能使 R18 的 tensor 传输成功。

DI 负责紧凑表示及恢复。主工作区的候选改动仍为待审查状态：
`TensorBundleCodec`、`ProviderGroupCoordinator` 和
`NdnsfCollaborationDependencyIo` 必须保留签名原文、精确名称、
有序 ciphertext 承诺、AEAD/HMAC 和完整内容摘要的校验。
旧格式保持原始字段检查；只有紧凑格式才从已认证 Selection/能力恢复
省略字段。不能覆盖旧字段后再把自相等判断称为身份验证。

当前源码审查发现：候选消费代码无条件覆盖 inner manifest/descriptor，
随后无条件比较仅紧凑格式才有的 transportManifestDigest，破坏旧格式
兼容；候选外层获取尚需在分配/获取前检查 operation/edge/capability
的字节、分段数与算术边界。修复这些问题后才可采用候选代码。
通用层不纳入候选文件中的 YOLO Merge 名称特判。

## Focused Verification Plan

Core 使用真实 ServiceProvider、签名密钥、IMS 和 DummyClientFace
测试 8,799 / 8,800 / 8,801 字节的完整 Data：前两者可发布且可拉取，
后一者拒绝；批次末项超限时前项也不能出现在缓存。先在提交前实现
运行同一断言，再修复并重跑，保留独立 raw 目录。

DI 使用生产发布/消费路径验证大 tensor 的完整签名包大小、内容往返、
紧凑与旧格式兼容，以及签名/承诺/身份/边界变异拒绝。不能只检查
content 字节数或用手工恢复的测试副本代替真实消费路径。
随后刷新完整 native identity，复审 A05，再开始新的同源正式矩阵。

## Status

`IN_PROGRESS`：Core R1 语义 RED、R2 修复后 21/21 断言 PASS；
下一步修复 DI 紧凑表示及生产消费，随后整体 native 刷新与复审。
T005/T008 未完成；A05 保持 BLOCK。

## Core Regression R1

原始目录为 ignored workspace temporary directory 下
`spec181-exact-wire-core-20260906-r1/`；源码基线 `b95b7e84`，仅新增
测试及构建注册。系统 Python/Waf `--targets=spec181-exact-data-wire -j2`
构建 exit 0（19.284 s），链接已验证的生产 Core shared library。
以私有 HOME、memory PIB/TPM、未使用的 transport 运行
`--run_test=Spec181ExactDataWire --report_level=detailed --log_level=test_suite`。

测试 exit 201：18/21 断言通过，3 项语义断言失败（约 0.506 s）：
8,801-byte 对象错误返回成功，末项超限的批次错误返回成功，批次
前项仍能由消费端经真实 IMS 拉取。8,799/8,800-byte 的发布、完整
签名包大小、内容拉取及后续正常发布均通过。这不是构建或启动失败。
采用候选 Core 大小预校验后，以原样测试在新的 R2 目录验证。

## Core Repair R2

原始目录 `spec181-exact-wire-core-20260906-r2/`。仅采用 Core 的
PreparedData 批量预校验 hunk；所有对象先签名并测量完整 wire，任一
超限就返回 false，全部通过才插入 IMS。主工作区与隔离检出的该
方法字节一致。测试和构建注册与 R1 相同，测试未放宽。

相同 Waf target 重建生产 Core 与测试 executable，exit 0
（1m34.961s）。相同测试命令 exit 0：1 个生产路径用例、21/21
断言 PASS（约 0.750 s）。确认实际发送的 8,799/8,800-byte 包
可拉取；8,801-byte 包被拒绝；末项超限批次的前项不能拉取；
后续正常发布恢复成功。未运行完整本地清单或 MiniNDN。

Core 定向边界 PASS。新增测试同时注册到维护 integration-tests
以保留 T008 覆盖。DI 格式/消费缺口仍 BLOCK，完整 native identity
需在该共享传输修复后刷新；不能使用 R2 作为正式资格。

## Documentary Check

首次 inventory 渲染 exit 1：本新增记录缺少头部 Evidence layer，
触发 `ACTIVE_EVIDENCE_LAYER_MISSING`。这是文档元数据缺项，不影响
R2 运行结果；原始错误保留于 R2 的 `inventory-initial.log`。补充
显式层级和状态后重新生成、检查 inventory，不修改门禁脚本。
