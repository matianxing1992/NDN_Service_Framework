# Data Model

## TurnTiming

身份：run/candidate、providerBootId、conversationId、requestId、attempt、role、tokenIndex。
阶段：submit、requestPublished、ackReceived、ackVerified、ackClosed、planBegin/End、
selectionCommitted、sessionAcquire/Ready、ortRunBegin/End、tokenReceived/Emitted、
checkpointCommitted、turnReady、closeBegin、drainEnd。
本地持续时间使用 steady clock；wall clock 仅关联同机日志。缺事件标 missing，不能填零。
prefill 与 decode 分开，EOS 是否计数明确；单一请求不能混入重试 attempt 的事件。

## AckWindowPolicy

Qwen profile `ack_timeout_ms=1000`，显式覆盖正整数且小于 request total timeout。
requestPublished 开始计窗；bootstrap/readiness 独立。deadline 冻结一次，迟到 ACK 无权修改集合。
pending-verification、invalid、negative、duplicate、missing-role 单独计数；遵守原有有界认证收尾。

## LoadedSessionIdentity and SessionLease

内容：assembled bytes 与外部权重摘要、recipe/graph/initializer/adapter 摘要、role、
IO/state/position contract 摘要、backend ABI/ORT版本、实际provider/device、precision/layout/
quantization、security domain与protection epoch。canonical 字段长度编码后计算SHA256，不能拼接歧义。
不使用当前 requestId/planDigest/profilePath 作为 immutable key，也不把它们移入缓存。
entry：identity、loadId、backing owner、session、load evidence、active leases、lastRelease、state。
state：Loading→Resident→Retiring→Destroyed；加载失败不缓存，close 后不能开始新加载。
同key并发只加载一次，等待可取消且受deadline约束；失败唤醒全部等待者。
本Spec190 CPU多轮profile显式启用后，默认最多1个loaded identity/Provider、idle TTL120秒；
其他profile和原无cache入口不自动启用/限槽。替换先retire空闲项，在用容量不够则有界等待/
明确资源拒绝，不能悄悄再加载超预算副本。会话KV的有效期不由模型TTL延长。
模型bytes统计不等于ORT/RSS用量：另记进程RSS/available/swap，测试owner释放而非强迫allocator立即归还OS。

## Evidence Separation

LoadEvidence：loadId、完整identity、Provider boot、实际backend/device、首次真实profile及其原request/attempt。
RequestExecutionObservation：当前request/attempt/plan/role、loadId、实际run计数/起止、executionCompleted。
cacheHit为观察字段，不是授权、KV命中或executionCompleted。请求可变状态留在新runner；CPU KV沿用原生独立store。
