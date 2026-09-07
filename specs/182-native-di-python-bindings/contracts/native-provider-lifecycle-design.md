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

T009新增真实ExecutionLeaseService/Core table单测：两个targets争同一槽只允许一个Prepare；另一target不能Commit/Abort/Renew/Release前者lease；关闭A后B继续Prepare/执行；A只允许清理原lease，关闭不能提前释放执行中槽；重复serve不覆盖固定lease handler。保留原单target constructor全部测试。拟新增选择器`Spec182SharedLeaseCrossServiceConflict`、`Spec182SharedLeaseTargetBinding`、`Spec182ClosingServicePreservesSharedLeaseOwner`；T009绑定到现有execution lease unit注册，T016执行真实双服务PO-014。当前均NOT_RUN。
