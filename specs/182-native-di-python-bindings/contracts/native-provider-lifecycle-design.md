# Native Provider Registration and Shared Lease Design

**Status**: PARTIAL / T001 IN_PROGRESS / T009 BLOCK
**Scope**: CD-014 / O-004 / PO-014

## Verified Registration Boundary

`ndn-service-framework/ServiceProvider.cpp::addService`写入`m_services[serviceName]`，`addCollaborationHandler`覆盖`m_collaborationServices[serviceName]`；两者没有返回registration token，也没有公开remove/unregister。因此“调用add即完成安全重复注册”和“close包装已有unregister”都不成立。Core maps上的修改应沿其既有Face线程约束；不能在DI RAII析构线程直接擦Core内部map。

`examples/DI_NativeProviderExecutable.cpp`目前为单个options.serviceName创建ExecutionLeaseService，然后在固定EXECUTION_LEASE_SERVICE_NAME注册NormalAndTargeted入口；随后注册该推理服务的collaboration。把这段代码按每个serve重复执行会覆盖前一个lease handler。即使分发至多个原实例，每个实例私有m_table也会让相同provider:compute-slot:N在不同表分别成功预留。该结论是对拟议多服务提取的源码推导；没有运行多服务native测试，不称现有单服务实验失败。

## Shared Lease Ownership

T009的NativeInferenceProvider宿主持有一个共享lease状态、一个固定lease入口和多个target service记录。推理服务关闭不销毁其他服务的lease表、Provider、Face或KeyChain。复用现有Core ProviderExecutionLeaseTable和wire schema，不另建租约协议。

| File / symbol | Exact planned change |
| --- | --- |
| `NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionLeaseService.hpp` | 新增`SharedExecutionLeaseState`，字段`ProviderExecutionLeaseTable table`及`mutex prepareMutex`，构造接收providerEpoch。新增ExecutionLeaseService overload `(providerName,targetServiceName,resolver,shared_ptr<SharedExecutionLeaseState>)`；原四参数constructor保留并委托自有shared state |
| 同类private字段 | 将value `m_table`及`m_prepareMutex`替换为非空`shared_ptr<SharedExecutionLeaseState> m_sharedState`；保留m_providerName、m_targetServiceName、m_conflictKeyResolver。实例仍绑定单target，避免一次修改全部调用方 |
| `ExecutionLeaseService.cpp::handle` | prepare在shared prepareMutex内完成resolver+table.prepare，所有targets串行选择共享资源；其他操作调用shared table。针对已存在lease，在非Prepare操作前用find检查lease.serviceName与m_targetServiceName一致，跨target返回LEASE_SERVICE_MISMATCH；随后仍由Core检查requester/epoch/idempotency，不以find成功替代授权 |
| `ExecutionLeaseService.cpp::table` | 保留现有返回类型和noexcept，返回m_sharedState->table；调用方持有service owner时引用有效 |
| `NativeInferenceProvider.cpp` (planned) | 只注册一次固定lease handler；解析已有request.targetServiceName并路由到该target的ExecutionLeaseService，所有实例共享一个state。保留Core签名/加密/权限入口、原response编码与指标；不按每服务重新创建资源表 |
| `examples/DI_NativeProviderExecutable.cpp` | 将现有lease注册、resolver与collaboration初始化提取到公共host；CLI调用host，不保留独立运行分支。binding和独立consumer均消费同一host |

shared state的providerEpoch为同一host boot epoch；不同target不能通过单独重置表绕过已预留资源。计算槽范围是host实际资源配置，不能为每个service重新增加workers；同一物理槽使用相同conflict key。后续serve配置若试图改变host槽范围必须拒绝并要求显式host资源配置，不能默默相加。Core prepare自身的冲突检查保留，shared mutex只确保resolver观察和prepare之间不被另一个target抢占。

非Prepare请求的targetServiceName不能任意改写路由：当前Core commit/abort/renew/release签名只有leaseId/providerEpoch/requester/idempotency，没有target参数。共享表后额外serviceName检查因此是必要边界。未知lease保持Core既有缺失/过期处理；已存在记录先核对target，再交Core处理身份、状态与重放。不得通过router返回另一服务的lease细节。

## Closing Targets

router中每个target记录active/draining状态。close立即停止该target的新Prepare、Commit、Renew；Abort/Release继续路由已有记录完成清理，其他targets不变。在途推理通过既有executionGuard/deadline停止并在安全释放点归还资源，不能close时直接把仍运行的槽标成空闲。draining记录及shared state保留到在途引用与有效lease结束；缺少安全可证明的清理条件时宁可保留有界TTL记录，不允许新服务盗用旧执行资源。

同名重新serve不得复用draining记录中的旧授权、准备工件或旧request绑定。精确registration generation、ACK/Selection旧请求fence以及Core入口释放方案仍是本契约下一闭合项；本节只定义lease所有权，**不宣称NativeServiceRegistration::close已经可实现或T009可开始**。

## Required Tests

### Registration Generation Decision

2026-09-07追加源码证据：`ServiceProvider.cpp::dispatchAckDecisionAsync`复制ACK handler到worker，完成后post至Face调用finishAckDecisionOnEventLoop；Selection路径及dispatchCollaborationExecutionAsync重新查找m_collaborationServices中的当前handler。因此只给DI callback包一层closed bool不能阻止旧已接受Request被新代handler执行。必须由Core把注册代次固定到本地pending request，并在ACK发布、Selection派发和既有CollaborationWorkFence.current检查；此代次是本地生命周期元数据，不增加NDN wire字段或新的授权机制。

选用Core显式scoped registration扩展，保留所有legacy addService/addCollaborationHandler签名与行为；DI host改用新增API，不通过覆盖旧entry实现close。以下为T009的必需Core改动，不是当前已有能力。

| File / interface | Planned responsibility and fields |
| --- | --- |
| `ndn-service-framework/ServiceProvider.hpp` | 新增嵌套`ServiceRegistration` RAII handle，move-only，`close() noexcept`幂等；内部共享RegistrationState，不持有NDNSF-DI类型。state含非零generation、serviceName、atomic closed及受控清理入口；generation由Provider单调分配，溢出拒绝 |
| `ServiceProvider::addScopedService(serviceName,ackHandler,requestHandler,invocationMode) -> ServiceRegistration` | 注册lease入口，原参数类型不改；serviceName已由任意现存注册占用时拒绝，不覆盖普通应用服务 |
| `ServiceProvider::addScopedCollaborationHandler(serviceName,allowedRoles,ackHandler,handler) -> ServiceRegistration` | 注册推理服务，重用原角色/权限/签名路径；active重名拒绝，旧代已closed且Core清理完成才允许新代 |
| `ServiceProvider.cpp` / registered records | RegisteredService与RegisteredCollaborationService新增optional shared RegistrationState；普通legacy entry为null。legacy add试图覆盖active scoped entry也拒绝，不能绕过scoped独占检查 |
| pending request metadata | 新增pendingKey→shared RegistrationState映射，与pendingRequests同锁/TTL/cleanup生命周期；认证Request选中scoped handler时固定state，只要该pendingKey仍存在就不重新绑定新代 |
| ACK completion | dispatchAckDecisionAsync任务捕获固定state；执行前和Face发布前检查closed；finishAckDecisionOnEventLoop只接受与pending固定state一致的结果。关闭后的结果不得重新写pending或发positive ACK，沿已有拒绝/清理路径结束 |
| Selection dispatch | 比较pending固定state与当前entry state相同且未closed，失败用既有selection failure路径；不得返回false退入legacy/default handler。同步fallback与异步路径必须相同 |
| CollaborationWorkFence | makeCollaborationWorkFence在现有deadline/controller/request检查上加入捕获state未closed；worker不读取可变注册map，所有后续I/O/排队执行继续复用当前fence |
| cleanupPendingRequestState | 同步释放pending registration引用，保留原replay tombstone语义；close不能为允许新代而提前清除旧请求重放记录 |

close首先原子关闭state，使新ACK/Selection和排队执行立即无效，再在Face序列化清理所属entry。清理只能删除仍指向同一state的entry；旧handle不能删除新代。只有该服务名没有其他合法entry时才移除m_serviceNames项；不能修改其他服务或停止共享Face。Core entry和已排队任务各持有必要shared state/handler至安全释放，避免裸DI host指针；callback仅weak引用host注册记录，进入执行时取得shared owner并检查closed。

新增registration API沿Core现有注册线程约束：只能在Face事件线程或event loop启动前调用。DI `serve`执行同样约束，错误线程明确拒绝；不在Face上等待同步post，也不从析构线程直接改map。close可从任意线程触发，但posted清理必须使用Provider存活控制，不能只捕获裸this。Provider析构先使存活控制失效，再排空既有worker；关闭handle在Provider销毁后是无害操作。这一存活控制的具体实现复用/扩展现有shared stopping机制前，T009静态审查须证明无检查后析构竞争；不能只凭一个atomic检查声称无UAF。

重新serve等待旧代Core entry清理完成，而不是等待所有旧模型执行不可取消调用同步返回；旧执行始终持旧closed fence并在可控边界退出，资源由原owner清理。同名新代仍不得继承旧lease或正在执行的槽。close不承诺立即终止第三方不可取消调用，硬超时/worker收束按现有runtime契约。运行中的其他服务保持自己的state、pending和资源。

以上已确定隔离机制及Core改动范围；具体Provider存活控制、host配置线程判定与全部同步fallback位置仍需T001按源码补齐，故T009继续BLOCK，不能仅凭本节提前实施。

T009新增真实ExecutionLeaseService/Core table单测：两个targets争同一槽只允许一个Prepare；另一target不能Commit/Abort/Renew/Release前者lease；关闭A后B继续Prepare/执行；A只允许清理原lease，关闭不能提前释放执行中槽；重复serve不覆盖固定lease handler。保留原单target constructor全部测试。拟新增选择器`Spec182SharedLeaseCrossServiceConflict`、`Spec182SharedLeaseTargetBinding`、`Spec182ClosingServicePreservesSharedLeaseOwner`；T009绑定到现有execution lease unit注册，T016执行真实双服务PO-014。当前均NOT_RUN。
