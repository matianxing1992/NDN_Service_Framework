# Data Model

## ExperimentProfile

字段和类型权威为 [profile contract](contracts/experiment-profile.md)。一份 profile 引用 immutable inputs；每个注册 case 的变化显式可见。Profile/ResolvedRun/Candidate 不保存私钥。

## CandidateManifest

inputId I → runtimeId R → experimentId E 的不可变链，存 source lock、source seal、dependency/version/library inventory、SIF、harness、profile/model/oracle/security/contract hashes。阶段不产生 self-hash 循环。证据引用对应身份，不把所有历史 PASS 简单合并。

目标 `layered-v1`：I/R只拥有基础输入与SIF，E额外绑定外部AppManifest；现有旧
布局仍待迁移，旧身份不能重解释。应用变动只产生新AppManifest/E，不重建未变R。

## AppManifest

PLANNED，T002/T004/T011拥有。记录layoutVersion、app source revisions及文件seal、
产物路径/size/hash、entrypoints、app自有DSO/Python闭包、requiredRuntimeId/SIF hash、
SDK/compiler/build flags/dependency lock与构建回执。仅应用源/产物属于此清单；
基础库与通用绑定变化属于I/R。摘要不包含E自身。执行时固定只读`/app`，
模型/秘密/结果不混入app包；不允许同名基础库遮蔽。

## ResolvedRun

`runId`, `candidateId`, `caseId`, `effectiveCaseDigest`, `mode`, `allocation/jobId`, `nodeRank/hostname/IP`, `gpuUuid/visibleOrdinal/backend`, `role/identityDigest`, `resolvedPaths`, `namespace`, `startup/request/progress/cleanup deadlines`, `startedAt`, `finishedAt`。run ID 严格校验且不得覆盖已存在目录。
Controller 私钥不分发到 Provider/User；新角色材料的公开摘要记入本 run，生命周期结束安全清除私有材料，保留公开信任证据。

## RequestEvidence

`requestId`, `attemptId`, `warmup`, `planDigest`, `selectedProviderByRole`, `inputDigest`, `edge producer/consumer/name/digest`, `role start/end/backend/device`, `responseDigest`, `numerical.matched/shape/expectedShape/atol/rtol/maxAbsError`, `latencyMs`, `terminalReason`。事件允许合法时序重排，但不能混 request/attempt/epoch 或从另一 run 借输出。

## GateReceipt And RunResult

GateReceipt：`gate`, `candidateId`, `command`, `exitCode`, `status`, `evidencePaths/hashes`, `environment`, `auditDigest`；内容必须来自执行，不接受仅人工 PASS。
RunResult：ResolvedRun + 全部 RequestEvidence + 每 worker/child 退出/停止原因 + cleanup + firstFailure + summary。期望负例用 `EXPECTED_REJECTION_PASS` 与正例 inference PASS 分开。单节点、local、正式双节点类别不可升级。
正常结果必须完整满足 `validation-matrix.md`；晚到数据追加独立观察，不覆盖先前终态。重分析写另一个文件，记录分析器版本和原证据 hash。
