# DistributedRepo Two-Mode Analysis

状态：PROPOSED；2026-09-19。本轮只分析，不修改代码、已接受的高层原则或功能验收状态。

## 1. 结论与模式边界

这个划分合理。建议使用 **in-app mode / server mode** 描述运行和责任边界，不直接把它们等同于 memory / disk 或 transient / persistent。

**in-app Repo 是应用拥有的临时数据源；server Repo 是独立运行、承担约定保存责任的存储服务。** 两者共享存储与原始 Data 读取实现，但不共享所有网络管理权限和后台任务。

| 行为 | in-app mode | server mode |
|---|---|---|
| 生命周期 | 随所属应用启动、关闭 | 独立进程，支持重启恢复 |
| 所属应用通过本地 API 保存自己的数据 | 支持 | 支持受控管理入口 |
| 响应 Interest，提供已有 Data | 支持；受发布范围和生命周期限制 | 支持 |
| 接受外部 INSERT、STORE、上传会话 | 不支持 | 授权、选择及容量检查通过后支持 |
| 被选为副本存储目标 | 不允许 | 符合保存策略时允许 |
| 主动维护副本数量、后台修复 | 不承担 | 由明确的修复策略和责任者承担 |
| 计入要求的持久副本数 | 不计入 | 仅已提交且符合保存契约的副本计入 |
| 存储后端 | 可用内存或有界磁盘暂存 | 需要能满足所声明持久性和恢复要求的后端 |
| 对外声明 | 临时可读取来源，不声明持久存储能力 | 存储能力、容量、保存条件与定位信息 |

“不接受插入”应限定为**不接受外部管理操作**，否则应用自己也无法把摄像头帧、模型材料等放进组件。“不会 replicated”建议解释为不作为副本目标、不自动维护复制，而不是禁止服务器读取其原始数据。否则临时生产者无法把数据交给长期 Repo。

应用若明确需要归档，可使用独立 RepoClient 请求 server 保存；这是应用显式发起的归档，不是 in-app Repo 的自动复制责任。如果你的意思是连这种显式归档也禁止，需要另行收紧契约。

## 2. 当前代码具备什么，尚不能证明什么

当前已经存在部分基础，但不是只重命名一个字段就能完成两模式：

- `NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp:196` 的 StorageCapability 有 `repoMode`、`acceptsBackupReplica`，默认分别为 `persistent`、true。
- `pythonWrapper/py_repoclient/orchestration.py:3642` 的修复候选过滤检查 persistent 和接受副本；`:5426` 的目录同步启动也限制 persistent。
- 同文件 `:8330` 的 `select_replicas()` 只检查候选名称、接受副本标记和容量，没有同时检查模式。因此不能依赖模式字符串自然阻止副本选择。
- 同文件 `:4850` 的 INSERT 分支检查 replicaNodes 并拉取、校验和保存内容；该分支自身没有显式模式拒绝。是否存在所有入口共用的上层拦截仍须完整调用链审查，不能据此直接宣称安全漏洞或已有完整保护。
- `RepoDataReference` 已有 forwardingHint；字段存在不等于本机路由、远端路由、原始 packet serving 和恢复路径均已验收。

建议统一由模式导出操作能力，而非允许用户任意组合互相矛盾的布尔开关。客户端筛选只提高效率；接收端仍须独立拒绝禁止的操作。检查范围应包括普通 INSERT、STORE、STORE_PACKETS、分段/范围上传、恢复上传、修复入口和兼容路径。

## 3. 可以借鉴哪些现有 NDN Repo 机制

### repo-ng：借鉴插入生命周期，不照搬旧消息格式

repo-ng 将读取与管理分开：授权插入命令让 Repo 主动获取并存储命名 Data，客户端另行检查插入进度。这支持“被授权接受请求”和“所有数据已经保存”分开处理。其文档使用旧式 Signed Interest 命令，且状态码表与部分步骤存在不一致，因此只借鉴生命周期，不逐字照搬状态码或旧 wire format。[Repo Protocol Specification](https://redmine.named-data.net/projects/repo-ng/wiki/Repo_Protocol_Specification)、[Basic Repo Insertion Protocol](https://redmine.named-data.net/projects/repo-ng/wiki/Basic_Repo_Insertion_Protocol)。

### ndn-python-repo：与 Data-driven 管理消息更接近

其命令和状态通常是通过 PubSub 传递的 Data，插入参数包含对象名称、分段范围、可选 forwarding hint 和本地注册前缀；另外提供进度检查。可以借鉴“命令对象引用待取数据、Repo 按名称拉取”的分离，而不需要用其 PubSub 替换 NDNSF 的调用协议。[Encoding](https://ndn-python-repo.readthedocs.io/en/latest/src/specification/encoding.html)、[Insert](https://ndn-python-repo.readthedocs.io/en/latest/src/specification/insert.html)。

这些资料是协议和实现文档，不是对 NDNSF 两模式的性能验证；也不能据此推断现有 Repo 已具有我们要求的副本提交和恢复保证。本轮按学术检索技能核对原始规范，没有进行完整文献综述。

## 4. 建议的最小插入与复制流程

1. 应用将自己的已签名 Data 放入 in-app Repo，发布不可变 manifest；明确名称、内容摘要、分段范围、总大小和来源定位。
2. 需要长期保存时，由应用的 RepoClient 发起存储服务 Request，声明大小、保存期限和副本要求。只收集符合条件的 server 候选。
3. 选择所需 server；每个 server 收到绑定本次操作、对象身份和目标身份的 Selection 后，再确认授权并预留容量。ACK 是意愿/能力信息，不是已持久保存证明。
4. 被选 server 用 Interest 获取原始 Data，验证原生产者签名、manifest 绑定、范围、大小和摘要；保存原始 wire bytes，不改名、不代原生产者重签。
5. 完成后提交本地对象和索引，发布 server 签名的 receipt，绑定 operation ID、对象摘要、server 身份和约定保存条件。receipt 证明该 server 声明了什么，不是对未来可用性的密码学保证。
6. 客户端只按符合条件的提交 receipt 计算成功。需两个持久副本时，in-app 原件加一个 server 不算两个。
7. 响应丢失时查询同一 operation ID；同一身份和内容的重试幂等，不同内容冲突拒绝。部分成功必须可查询，不冒称分布式原子提交。

初版宜由客户端显式选择各副本并收集 receipt，避免服务器递归复制导致副本膨胀。后台修复属于后续独立责任：每个对象指定协调者或有界认领规则，记录唯一操作、预算和终态。源应用正常退出前可等待所需 receipt；异常退出早于持久提交则可能丢数据，协议不能消除这个窗口。

加密对象可以原样复制，存储服务器不必因此获得应用内容解密密钥。读取密文和读取明文权限不同；写入、删除、修复消耗资源，仍须分别授权。NDNSF 原始 packet 保存路径与兼容的 payload 重新封装路径也不能混称为同一种对象身份保证。

## 5. Interest 如何到达副本：ForwardingHint 合适，但不是完整方案

例如原始 Data 名称保持：

```text
/uav/drone7/camera/session42/frame108/seg=0
```

即使保存到地面 server，也不把它改名为 `/repo/ground1/uav/...`。消费者可以构造：

```text
Interest.Name = 原始 Data 名称
Interest.ForwardingHint = /site/ground1
```

ForwardingHint 提供转发位置提示，与所请求 Data 的 Name 分离；它不提供授权、复制、持久性或内容正确性保证。[NDN Interest specification](https://docs.named-data.net/NDN-packet-spec/current/interest.html)。

需完整实现三段：

1. **找到位置**：已验证 manifest/目录记录给出对象身份与候选副本 locator，记录发布者、版本和有效范围；客户端不得把任意未验证 locator 当成可信位置。
2. **到达位置**：网络具有 locator 前缀的可用路由。NFD 在进入配置的 producer region 后可移除 hint，改按原始名称转发。
3. **到达 Repo 进程**：所在节点/区域必须有原始数据前缀指向 Repo serving face 的本地路由和对应处理程序。仅注册 `/repo/ground1` 的普通名称过滤器，不能假定会接收名称为 `/uav/drone7/...` 的 Interest。[NFD NetworkRegionTable](https://docs.named-data.net/NFD/24.07/doxygen/classnfd_1_1_network_region_table.html)、[Forwarder](https://docs.named-data.net/NFD/24.07/doxygen/forwarder_8cpp_source.html)。

建议第一版使用“可路由 server/site locator + 受控的本地原名服务前缀”，不要默认在共享 NFD 注册 `/` 兜底，以免捕获无关流量。也不建议为每个 segment 注册路由，应选择可管理的对象/会话前缀并设置清理规则。

读取优先按目录和应用策略选择来源；某副本超时或不存在时，在总 deadline 内尝试其他已验证 locator。无须假定一个 hint 列表会自动完成满足业务要求的故障切换。返回 Data 沿 Interest 建立的转发状态返回，仍验证原生产者身份与内容摘要；不能因为来自 server 就跳过验证。

缓存可直接满足 Interest，因此普通成功读取不能证明指定 server 真正保存了副本；`MustBeFresh` 也不是绕过缓存的开关。副本提交与状态验收应使用操作绑定的 server receipt、存储状态和重启后取数证据。严格证明远端真实持有全部字节属于更强问题，不应默认声称已有。

## 6. 架构与最小验收

建议共享 RepoCore、manifest、存储后端和原名 Data serving；in-app 只组合本地写入、读取及生命周期管理；server 在此基础上增加外部写入门控、持久恢复、目录与受控修复。RepoClient 独立于两种服务端形态。避免复制两套存储实现，也避免把后台线程偷偷启动在嵌入式组件中。

实施前至少明确并验证：

- in-app 能保存和提供所属应用的 Data，但所有远端写入及副本请求均被接收端拒绝，拒绝后没有网络拉取、容量占用或后台任务。
- 客户端强行把 in-app 填入目标列表也不能绕过模式限制；它不计入持久副本完成条件。
- server 在提交前崩溃、提交后 receipt 丢失、重复操作、来源提前退出时，状态与资源可以解释和恢复。
- 原生产者离线且缓存未参与时，消费者能够通过 server locator 取回字节和签名均一致的原始 Data。
- hint 缺失/错误、路由缺失、目录过期、首个副本故障时，失败明确且有界。
- 取消、应用退出、过期清理不会删除仍有活跃读取约束的对象；过期与保护状态有明确优先级。

下一步应先确认上述模式契约及“应用显式归档是否允许”，再形成 API/协议状态机与逐入口差异清单；不应直接将本分析当成已实现设计写入 highlevel-design.md。本轮没有运行实验，也没有改变已有 Spec 的验收状态。
