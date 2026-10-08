# Data Model and State Invariants

## Existing Identity and Ownership

完整`ArtifactReference`八字段来自native header：`logicalName,digestAlgorithm,contentDigest,sizeBytes,formatVersion,rootManifestName,publisherIdentity,policyEpoch`。内容身份与placement分离；generic DI identity必须显式映射，不能只认同名类型。

`ArtifactHandle`关联reference、mode、immutable receipt和已认证selected locations；`ArtifactCommitReceipt`/per-replica receipt绑定operation/generation/reference/Repo/Provider/durable commit。CURRENT位置的version/时间/expected receipt独立，不从历史receipt推断仍在线。

In-App owner是应用native API+本地耐久root；Persistent owner是seed RepoNode+native metadata/payload store。ServiceUser借用/py strong owner有明确lifetime；Python不会成为durable store owner。

## Operation and Target State

operation seed屏障前不声称已接受后台工作；屏障后`ACCEPTED/IN_PROGRESS`不等于策略完成。`COMMITTED/PARTIAL/FAILED`终态由exact durable receipts和尝试记录生成，后续查询不可改写历史aggregate。

| Target condition | State / evidence | Required handling |
| --- | --- | --- |
| worker从未尝试该目标 | NOT_ATTEMPTED,count0 | aggregate可SKIPPED |
| pending目标曾有worker尝试、结果未确定 | IN_FLIGHT或终结UNKNOWN,count>0 | terminal unknown-outcome，不可变成SKIPPED |
| 明确失败 | FAILED +原failureCode | 保留attempt；本地源失败不抹除别的目标 |
| 认证durable commit | COMMITTED +exact receipt | 唯一可计achievedReplicas的事实 |

`attemptCount`记录worker尝试次数，不等于已发STORE次数；C的source materialization失败也可增加count而没有wire emission。请求是否发出必须用独立handler/packet证据判断，不能从count推断。不能用逻辑Repo别名增加Provider数量；late commit可在后续独立观测说明，不能回写已经冻结的terminal aggregate。

## Destination Commit and Recovery (planned)

状态为`STAGED_VERIFIED → PREVIOUS_PRESERVED → INSTALLED_UNACCEPTED → ACCEPTED`。普通失败在accepted前进入`RESTORED`；restore失败进入`RECOVERY_REQUIRED`并保留材料、停止retry。original absence同样是必须恢复的状态。accepted后的cleanup residue不修改successful result。

此内部状态不增加wire/schema或public枚举；精确路径/FD管理见DB002。POSIX fault恢复的return-time保证不等于power-loss保证。

## Availability and Original Data

`reachability`、`objectPresence`、`integrity`、`observedAt/validUntil/source`分别表达节点、对象与观测时效。artifact partial payload与传统segmented missing child不是同一catalog；分别验证，不让artifact reference借用传统manifest。

原始Data receipt绑定完整NDN name+wire digest+Repo/Provider+commit ID；current location认证且PRESENT/hint有效才能路由。producer签名证明原始producer/bytes，不证明哪台Repo这次回包；Provider响应元数据与现场packet/handler证明各自范围。

## Legacy Import Recovery

`STAGING/INCOMPLETE`是发布前状态；no-replace rename成功后destination可能可见但parent durability acknowledgment未建立。幂等重放验证same source/config/import identity/inventory并重试parent fsync；不能把COMMITTED manifest的存在单独当作刚失败屏障已经通过。

source DB/WAL/SHM/external CAS固定；旧replica/hint/operation策略保留为provenance，不作为认证current native commits。schema迁移不暗中扩大支持allowlist。
