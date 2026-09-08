# Value and Field Contracts

**Revision**: 7 | **Status**: DRAFT / PARTIAL

## Scope and Ownership

本表逐字段记录12个既有Python值类型；它们是迁移的语义来源，不是假称已经存在的C++声明。源码身份见[integrated baseline](integrated-baseline.md)。revision7已将137字段的名称、类型及默认值与当前源码AST逐项核对；来源字段不变，嵌套类型仍需O-004关闭。原声明保留实际默认值；未设默认的必填字段不得在C++中静默初始化为零值。所有planned native DTO均为值所有权：复制/移动拥有字符串、字节和容器；不能借用Python对象、span或短命回调栈。认证快照构造后只读；密钥不得进入这些DTO。

每行字段的英文 Doxygen 注释必须覆盖下表中文含义，并写明单位、来源、空值和消费者。Python docstring 说明兼容名称及值转换；不得把输入转换描述为授权或规划。容器字段在构造边界检查大小、唯一性和引用；所有 digest 复用既有规范算法/编码，无效值在 Request/commit 前拒绝。下列 mapping 仅保留来源字段语义；`Any`、嵌套 schema、精确 C++ 整数宽度及上限未闭合的行由 O-004 阻塞序列化，不能从名字猜测 ABI。

## V01 ModelDescriptor

Source: `NDNSF-DistributedInference/ndnsf_distributed_inference/splitter.py:142` → planned `NativeModelDescriptor`。

| Field ID | Existing declaration | Meaning and native consumer obligation |
| --- | --- | --- |
| V01.F01 | `model_name: str` | 模型目录的名称，用于查找；不能单独作为内容身份。 |
| V01.F02 | `content_digest: str` | 模型内容摘要，绑定认证工件与缓存键。 |
| V01.F03 | `semantics_digest: str` | 模型语义契约摘要，阻止相同权重配错任务。 |
| V01.F04 | `graph_digest: str` | 规范计算图摘要，连接候选、offer 与最终计划。 |
| V01.F05 | `model_format: str` | 已支持模型格式枚举，选择原生解析器。 |
| V01.F06 | `precision: str` | 权重/运算精度声明，匹配 Provider backend。 |
| V01.F07 | `adapter: AdapterDescriptor` | AdapterDescriptor 身份与版本，注册表查找入口。 |
| V01.F08 | `source_revision: str = ""` | 来源版本记录；空串保持旧兼容语义，不替代内容摘要。 |

## V02 ModelGraphSnapshot

原生图保留真实 tensor edges（producer/consumers 和 tensor ID），不可由相邻节点推断。
NativeTensorContract 的 shape 区分 int64 与 symbolic string，estimatedBytes 可未知；
NativeGraphEdge 的 tensor.name 必须等于 edge ID。节点引用、consumer 唯一性/拓扑
方向、合法 cut 引用均由 graph.validate 检查；见 [tensor edge audit](../evidence/t003-yolo-tensor-edges-20260907.md)。

Source: `NDNSF-DistributedInference/ndnsf_distributed_inference/splitter.py:211` → planned `NativeGraphSnapshot`。

| Field ID | Existing declaration | Meaning and native consumer obligation |
| --- | --- | --- |
| V02.F01 | `graph_digest: str` | 规范计算图摘要，连接候选、offer 与最终计划。 |
| V02.F02 | `adapter: AdapterDescriptor` | AdapterDescriptor 身份与版本，注册表查找入口。 |
| V02.F03 | `nodes: tuple[GraphNodeView, ...]` | 图节点视图集合，构建 role 覆盖校验。 |
| V02.F04 | `edges: tuple[TensorEdgeView, ...]` | 带类型/形状的张量边集合，决定跨角色数据流。 |
| V02.F05 | `topological_order: tuple[str, ...]` | 节点拓扑顺序，保证确定的遍历和切分顺序。 |
| V02.F06 | `legal_cut_edges: tuple[str, ...]` | 允许切分的边标识白名单，拒绝不可分割边。 |
| V02.F07 | `model_inputs: tuple[TensorContract, ...]` | 外部输入 TensorContract，验证 ingress。 |
| V02.F08 | `model_outputs: tuple[TensorContract, ...]` | 模型输出 TensorContract，验证终端结果。 |

## V03 SplitCandidate

NativeSplitCandidate.nodeRoles 保存 Python execution_plan.node_roles 的规划节点归属，
不写入旧 runtime ExecutionPlan wire。它必须覆盖 graph 中每个节点，且其 role 值
恰好覆盖声明角色；角色依赖必须无环。planning node ID 不等于 canonical ONNX
node index，实际装配映射仍由 adapter owner 提供，不能用 ordinal 直接替代。
roleStateInputsByRole/roleStateOutputsByRole 保存模型声明的 TensorContract 序列；
两者同时为空或完整覆盖角色，每角色非空、同侧 name 唯一。Qwen 填充维护 splitter
的 attention KV/recurrent/convolution 状态，YOLO 无状态时保留空映射。

候选边界必须调用 graph.validate(model)，拒绝非法/重复 cut，并核对 dependencies
中的 tensor 集合等于 crossPartitionTensors。rank 元数据出现时 degree/rank-artifact
均须完整覆盖角色，rank 工件数量等于 degree、互不重复且属于对应 artifacts。
此共享校验适用于默认和注入策略，不由准备/发布 owner 猜测修补。

Source: `NDNSF-DistributedInference/ndnsf_distributed_inference/splitter.py:341` → planned `NativeSplitCandidate`。

| Field ID | Existing declaration | Meaning and native consumer obligation |
| --- | --- | --- |
| V03.F01 | `source: SplitSource \| str` | 候选来源枚举，区分预切分工件与动态方案。 |
| V03.F02 | `splitter: SplitterDescriptor` | 切分器身份、版本和参数，参与候选可复现性。 |
| V03.F03 | `model: ModelDescriptor` | 模型引用/描述，绑定本次请求或候选的目标。 |
| V03.F04 | `graph_digest: str` | 规范计算图摘要，连接候选、offer 与最终计划。 |
| V03.F05 | `execution_plan: RoleExecutionPlan` | RoleExecutionPlan，定义角色及其执行关系。 |
| V03.F06 | `fragments_by_role: Mapping[str, str]` | 角色到 fragment 标识映射，查找对应子图。 |
| V03.F07 | `artifacts_by_role: Mapping[str, tuple[str, ...]]` | 角色到工件摘要有序集合，控制所需加载物。 |
| V03.F08 | `requirements_by_role: Mapping[str, RoleResourceRequirement]` | 角色到资源需求映射，匹配可执行 Provider。 |
| V03.F09 | `cross_partition_tensors: tuple[str, ...]` | 跨分区张量名集合，生成发布/拉取端点。 |
| V03.F10 | `estimated_costs: Mapping[str, int \| float \| None]` | 具名成本估计；None 表示未知，不能当零成本。 |
| V03.F11 | `tensor_degrees_by_role: Mapping[str, int] = field(default_factory=dict)` | 角色的张量并行度；仅接受现有支持范围。 |
| V03.F12 | `rank_artifact_digests_by_role: Mapping[str, tuple[str, ...]] = field( default_factory=dict)` | 角色按 rank 排列的工件摘要，验证精确 rank cover。 |
| V03.F13 | `role_state_inputs_by_role: Mapping[str, tuple[TensorContract, ...]] = field( default_factory=dict)` | 角色输入状态张量契约，连接续接/反馈。 |
| V03.F14 | `role_state_outputs_by_role: Mapping[str, tuple[TensorContract, ...]] = field( default_factory=dict)` | 角色输出状态张量契约，验证可提交状态。 |
| V03.F15 | `hybrid_plan: Any \| None = None` | 可选混合执行计划；禁止将 Any 直接迁入公开 C++ ABI。 |
| V03.F16 | `selection_priority: int = 0` | 候选优先级，进入既有确定性排序规则。 |
| V03.F17 | `input_ingress_role: str = ""` | 唯一输入接入角色名，必须属于 roles。 |
| V03.F18 | `result_egress_role: str = ""` | 唯一最终结果角色名，必须属于 roles。 |
| V03.F19 | `merge_kind: str = ""` | 结果合并方式，限定 adapter 已支持枚举。 |
| V03.F20 | `postprocessing: Mapping[str, Any] = field(default_factory=dict)` | 任务后处理参数；冻结键和类型后交原生 adapter 使用。 |

## V04 CandidateBudget

Source: `NDNSF-DistributedInference/ndnsf_distributed_inference/core/ports.py:114` → planned `NativeCandidateBudget`。

| Field ID | Existing declaration | Meaning and native consumer obligation |
| --- | --- | --- |
| V04.F01 | `max_candidates: int` | 本轮最多枚举候选数；有界计数，不表示角色数。 |
| V04.F02 | `max_policy_ms: int = 100` | 规划策略耗时上限，单位毫秒，默认100；使用单调时钟。 |
| V04.F03 | `max_reentries: int = 1` | 规划允许重入次数，默认1；不是网络 attempt 数或无限重试开关。 |

## V05 InferenceTaskDescriptor

Source: `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/base.py:62` → planned `NativeRequestOptions.task`。

| Field ID | Existing declaration | Meaning and native consumer obligation |
| --- | --- | --- |
| V05.F01 | `task_name: str` | 任务名称，选择输入/输出语义契约。 |
| V05.F02 | `input_schema_digest: str` | 输入编码 schema 摘要，编码与解码须一致。 |
| V05.F03 | `options_schema_digest: str` | 任务选项 schema 摘要，拒绝未知参数编码。 |
| V05.F04 | `result_schema_digest: str` | 结果 schema 摘要，校验 decodeResult 输出。 |

## V06 ApplicationInput

Source: `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/base.py:78` → planned `NativeApplicationInput`。

| Field ID | Existing declaration | Meaning and native consumer obligation |
| --- | --- | --- |
| V06.F01 | `task_name: str` | 任务名称，选择输入/输出语义契约。 |
| V06.F02 | `input_schema_digest: str` | 输入编码 schema 摘要，编码与解码须一致。 |
| V06.F03 | `options_schema_digest: str` | 任务选项 schema 摘要，拒绝未知参数编码。 |
| V06.F04 | `payload: bytes` | 输入编码字节，由原生 adapter 生成；有界且不可写别名。 |
| V06.F05 | `options: bytes` | 任务选项；ApplicationInput 为编码字节，GenerationRequest 为 TaskOptions 引用，必须区分。 |
| V06.F06 | `metadata: Mapping[str, str] = field(default_factory=dict)` | 非秘密字符串元数据；限定键，不能携带授权覆盖值。 |
| V06.F07 | `transport_mode: InputTransportMode \| str = InputTransportMode.INLINE` | 输入承载方式枚举，默认INLINE；控制 inline 或认证引用。 |
| V06.F08 | `repo_reference: Mapping[str, Any] \| None = None` | 可选已认证仓库引用，只有引用模式允许；字段闭合前禁止接受任意 map。 |

## V07 InferenceStateContract

Source: `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/base.py:164` → planned `native state contract within prepared request`。

| Field ID | Existing declaration | Meaning and native consumer obligation |
| --- | --- | --- |
| V07.F01 | `profile: str` | 状态协议配置名称，选择估算/复用规则。 |
| V07.F02 | `state_class: InferenceStateClass \| str` | 状态类型枚举，区分无状态与可复用状态。 |
| V07.F03 | `identity_schema_digest: str` | 状态身份 schema 摘要，决定复用匹配。 |
| V07.F04 | `estimator_schema_digest: str` | 状态资源估算 schema 摘要，匹配预算。 |
| V07.F05 | `allowed_tiers: tuple[str, ...]` | 允许的存储层级集合，限制驻留/迁移。 |
| V07.F06 | `owner_scope: str` | 状态所有者范围，防止跨 owner 复用。 |
| V07.F07 | `role_scope: str` | 可消费状态的角色范围。 |
| V07.F08 | `confidentiality: str` | 状态保密级别，决定保护与清理要求。 |
| V07.F09 | `maximum_retention_ms: int` | 最大保留时长，毫秒；不能绕过 grant/lease 到期。 |
| V07.F10 | `eviction_policy: str` | 驱逐策略枚举，控制状态丢弃。 |
| V07.F11 | `boot_epoch_bound: bool` | 是否绑定 Provider 启动 epoch，重启后重新验证。 |
| V07.F12 | `cache_epoch_bound: bool` | 是否绑定缓存 epoch，失配时禁止复用。 |
| V07.F13 | `pin_required_for_reuse: bool` | 复用是否要求 pin，防止计划期间被驱逐。 |
| V07.F14 | `migration_supported: bool` | 是否允许既有状态迁移；true 不构成新增迁移算法。 |
| V07.F15 | `revalidation_rule: str` | 复用前重新验证规则标识。 |
| V07.F16 | `cleanup_rule: str` | 终态/到期清理规则标识。 |
| V07.F17 | `cross_security_domain: bool = False` | 是否允许跨安全域，默认false；不得通过绑定层放宽。 |

## V08 GenerationRequest

Source: `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py:325` → planned `NativeInferenceClient::request arguments / NativeRequestOptions`。

| Field ID | Existing declaration | Meaning and native consumer obligation |
| --- | --- | --- |
| V08.F01 | `model: ModelRef` | 模型引用/描述，绑定本次请求或候选的目标。 |
| V08.F02 | `task: InferenceTaskRef` | 任务引用，绑定模型支持的 task contract。 |
| V08.F03 | `input: ApplicationInput` | 应用输入值，拥有有效 payload 或认证引用。 |
| V08.F04 | `timeout_ms: int` | 请求总预算，毫秒；进入 native owner 后推导 deadline。 |
| V08.F05 | `options: TaskOptions \| None = None` | 任务选项；ApplicationInput 为编码字节，GenerationRequest 为 TaskOptions 引用，必须区分。 |
| V08.F06 | `objective: Any = None` | 放置目标参数；Any 必须冻结为已支持 typed variant。 |
| V08.F07 | `constraints: Mapping[str, Any] = field(default_factory=dict)` | 放置约束；逐键冻结单位/范围，不能传任意策略函数。 |
| V08.F08 | `request_id: str = ""` | 请求身份；旧入口允许空串，native由 owner 生成，禁止绑定层伪造。 |
| V08.F09 | `output_mode: str = "FULL"` | 结果形式，旧默认FULL；native仅接受兼容清单列出的值。 |

## V09 ProviderPlanningViewV3

本契约中 model/snapshot/candidate/offer/proposal/core 的顶层 graph_digest 是 planning
graph；角色装配 recipe 的 graph_digest 可为不同的 canonical ONNX graph。
NativeInspectedModel 与 NativeArtifactBinding 显式保留 canonicalGraphDigest，不能用
planning graph 代填 canonical 身份。发布后业务 root 变更另需 inspection 的
canonicalSourceBytes、可选 canonicalInitializerObjectDigest/canonicalInitializerBytes，
以及 publication 的 canonicalManifestJson 与 artifactNameByRole；后者与 sourceByRole
实际获取地址分开。验证和重新认证按 [runtime boundaries](runtime-boundaries.md) 执行；
两种图身份的来源见 [graph identity evidence](../evidence/t008-graph-identity-spaces-20260907.md)。

Source: `NDNSF-DistributedInference/ndnsf_distributed_inference/sdk/placement.py:789` → current
`NativeAdmittedOfferV3::observation()`；旧 `NativeProviderPlanningView` 不是完整 V3 字段载体。

| Field ID | Existing declaration | Meaning and native consumer obligation |
| --- | --- | --- |
| V09.F01 | `provider: str` | 已认证 Provider NDN 身份，匹配 offer/projection/grant。 |
| V09.F02 | `offer_digest: str` | 完整认证 offer 摘要，封闭 ACK provenance。 |
| V09.F03 | `request_id: str` | 请求身份；旧入口允许空串，native由 owner 生成，禁止绑定层伪造。 |
| V09.F04 | `attempt: int` | 本次协作尝试编号，由 Core/request owner 一处推进。 |
| V09.F05 | `topology: DeviceTopologyProfile` | 已认证设备拓扑，约束放置合法性。 |
| V09.F06 | `resources: Tuple[DeviceResourceSnapshot, ...]` | 设备资源快照集合；保留时间/单位与过期校验。 |
| V09.F07 | `residency: Tuple[ResidencyProofV3, ...]` | 精确驻留证明集合；has_model 不能替代该证据。 |
| V09.F08 | `accepted_roles: Tuple[str, ...]` | Provider 可接受的 role 集合，不能扩张为任意角色。 |
| V09.F09 | `backends: Tuple[str, ...]` | 已声明可用执行后端集合。 |
| V09.F10 | `execution_disposition: ExecutionDisposition` | 执行处置枚举；REJECT 必须被过滤。 |
| V09.F11 | `preparation_accepted: bool` | 是否接受所需准备，不能推断已经驻留。 |
| V09.F12 | `queue_depth: int` | 等待队列长度，非负计数。 |
| V09.F13 | `estimated_wait_ms: float` | 等待估计，毫秒；未知/非有限值按旧规则拒绝或标未知。 |
| V09.F14 | `rtt_ms: float` | 网络往返估计，毫秒；只参与排序，不能作为认证证据。 |
| V09.F15 | `bandwidth_mbps: float` | 带宽估计，兆比特/秒；不是 MB/s。 |
| V09.F16 | `boot_epoch: str = ""` | Provider 进程代际，参与驻留/状态有效性。 |
| V09.F17 | `model_digest: str = ""` | 模型内容身份，必须与目标模型一致。 |
| V09.F18 | `graph_digest: str = ""` | 规范计算图摘要，连接候选、offer 与最终计划。 |

## V10 PlacementProposalV3

Source: `NDNSF-DistributedInference/ndnsf_distributed_inference/sdk/placement.py:1428` → current
`NativeRolePlacementProposalV3` 的 context/roles/assignment/strategy；candidateDigest 和
dependencies 当前分别由 NativeSplitCandidate、NativeExecutionPlan 传给 V3 sealCore。
完整 requester 生成与绑定这些输入仍待接线；旧 NativePlacementProposal 不再是策略基类返回值。

| Field ID | Existing declaration | Meaning and native consumer obligation |
| --- | --- | --- |
| V10.F01 | `request_id: str` | 请求身份；旧入口允许空串，native由 owner 生成，禁止绑定层伪造。 |
| V10.F02 | `attempt: int` | 本次协作尝试编号，由 Core/request owner 一处推进。 |
| V10.F03 | `model_digest: str` | 模型内容身份，必须与目标模型一致。 |
| V10.F04 | `graph_digest: str` | 规范计算图摘要，连接候选、offer 与最终计划。 |
| V10.F05 | `roles: Tuple[RoleAssemblySpec, ...]` | RoleAssemblySpec 集合，要求无重复且覆盖完整。 |
| V10.F06 | `provider_by_role: Mapping[str, str]` | 角色到 Provider 的唯一分配，引用已接受 offer。 |
| V10.F07 | `dependencies: Tuple[Mapping[str, Any], ...] = ()` | 跨角色依赖；逐端点和张量契约校验，禁止任意 JSON 绕过。 |
| V10.F08 | `candidate_digest: str = ""` | 被选候选的规范摘要，防止 seal 时替换候选。 |
| V10.F09 | `strategy_name: str = ""` | 策略稳定名称，用于身份与复现。 |
| V10.F10 | `strategy_version: str = ""` | 策略语义版本，用于兼容和摘要。 |
| V10.F11 | `strategy_state_digest: str = ""` | 策略参数规范摘要；不能来自运行时可变全局状态。 |

## V11 GenerationExecutionContractV1

Source: `NDNSF-DistributedInference/ndnsf_distributed_inference/sdk/placement.py:1471` → planned `NativeGenerationOptions / native generation contract`。

| Field ID | Existing declaration | Meaning and native consumer obligation |
| --- | --- | --- |
| V11.F01 | `mode: str` | 生成执行模式，限定现有支持集合。 |
| V11.F02 | `max_generated_tokens: int` | 最多新增 token 数；正数且受请求/资源预算限制。 |
| V11.F03 | `token_input_name: str` | 生成 token 的模型输入张量名。 |
| V11.F04 | `state_input_names: Tuple[str, ...]` | 有序状态输入张量名，顺序与模型一致。 |
| V11.F05 | `state_output_names: Tuple[str, ...]` | 有序状态输出张量名，必须与反馈契约匹配。 |
| V11.F06 | `eos_token_ids: Tuple[int, ...]` | 终止 token ID 集合，按 tokenizer 词表验证。 |
| V11.F07 | `sampling_digest: str` | 完整采样配置规范摘要，保证续接一致。 |
| V11.F08 | `tokenizer_digest: str` | 固定 tokenizer 内容摘要，禁止运行时换词表。 |
| V11.F09 | `sampling_mode: str = "Greedy"` | 采样模式，默认Greedy；不扩大现有算法范围。 |
| V11.F10 | `sampling_temperature: float = 0.0` | 采样温度，默认0.0；与模式联合校验。 |
| V11.F11 | `sampling_top_k: int = 1` | top-k 参数，默认1；限定有效词表范围。 |
| V11.F12 | `sampling_top_p: float = 1.0` | top-p 参数，默认1.0；按现有采样契约校验范围。 |
| V11.F13 | `sampling_repetition_penalty: float = 1.0` | 重复惩罚系数，默认1.0；必须有限且有效。 |
| V11.F14 | `sampling_seed: int = 1_750_001` | 确定性采样种子，默认1750001；不用于安全随机数。 |
| V11.F15 | `stop_strings: Tuple[str, ...] = ()` | 文本停止串集合，按完整累计文本匹配，非逐token拼接判断。 |
| V11.F16 | `generation_id: str = ""` | 生成会话身份，连接 journal 和已提交前缀。 |
| V11.F17 | `committed_prefix_token_ids: Tuple[int, ...] = ()` | 已经提交的 token 前缀，恢复时不可重复提交。 |
| V11.F18 | `streaming_operation_stride: int = 0` | 流事件步长；0只是声明默认值，现有校验要求有效生成契约大于0，构造最终计划前必须填入合法值，不能把默认当作可执行配置。 |

## V12 PlacementPlanCoreV3

Source: `NDNSF-DistributedInference/ndnsf_distributed_inference/sdk/placement.py:1567` → planned `NativePlacementPlanCore`。

| Field ID | Existing declaration | Meaning and native consumer obligation |
| --- | --- | --- |
| V12.F01 | `request_id: str` | 请求身份；旧入口允许空串，native由 owner 生成，禁止绑定层伪造。 |
| V12.F02 | `attempt: int` | 本次协作尝试编号，由 Core/request owner 一处推进。 |
| V12.F03 | `model_digest: str` | 模型内容身份，必须与目标模型一致。 |
| V12.F04 | `graph_digest: str` | 规范计算图摘要，连接候选、offer 与最终计划。 |
| V12.F05 | `roles: Tuple[RoleAssemblySpec, ...]` | RoleAssemblySpec 集合，要求无重复且覆盖完整。 |
| V12.F06 | `provider_by_role: Mapping[str, str]` | 角色到 Provider 的唯一分配，引用已接受 offer。 |
| V12.F07 | `dependencies: Tuple[Mapping[str, Any], ...]` | 跨角色依赖；逐端点和张量契约校验，禁止任意 JSON 绕过。 |
| V12.F08 | `ack_closed_digest: str` | 本轮封闭 ACK 集摘要，seal 必须使用同一 snapshot。 |
| V12.F09 | `strategy_digest: str` | 最终策略身份摘要，防止准备和 commit 使用不同配置。 |
| V12.F10 | `plan_core_digest: str = ""` | 未含最终授权装饰的规范 core 摘要，由 sealer 计算。 |
| V12.F11 | `candidate_digest: str = ""` | 被选候选的规范摘要，防止 seal 时替换候选。 |
| V12.F12 | `request_contract_digest: str = ""` | 请求 task/input/options/state 契约摘要。 |
| V12.F13 | `generation_contract: GenerationExecutionContractV1 \| None = None` | 可选 GenerationExecutionContractV1；普通角色不伪造生成状态。 |

## Native Representation Closure

| Family | Required representation and use | Remaining decision / gate |
| --- | --- | --- |
| NativeModelRef / NativeInspectedModel | NativeModelRef 使用认证 model/adapter 引用，不重复保存可覆盖的摘要；inspect 产出 V01/V02 与已认证源引用。输入引用和解析结果分型。 | O-004：目录引用嵌套字段与证书绑定逐项映射 |
| NativePlanningSnapshot | 拥有 V01/V02、V09 集合、ACK digest、请求身份及单调 deadline；来自同一 ACK_CLOSED，不接收 mutable Provider 对象。 | O-004：Core ACK 到 V09 精确转换 |
| NativeStrategyIdentity | name、version、规范配置摘要；策略构造后不可变，参数变更须新实例。 | O-004：策略参数规范编码与旧算法映射 |
| NativeRequestOptions / NativeRequestControl | task、timeoutMs、ackTimeoutMs、可选 generation/continuation；control 持 requestId、attempt、deadline、取消/fencing generation。前者用户配置，后者 native owner 独占。 | C01/M02 及 state dictionary；禁止公开覆盖 ACK digest |
| NativeInferenceResult / NativeInferenceEvent / NativeRequestStatus | result 为已认证最终 payload 与执行元数据；event 为只读进度/文本快照；status 为 CD-007 单一终态机。原始异常/日志不是 Response。 | O-004：全部事件枚举及 Python 错误/返回兼容映射 |
| NativeDiError | code、domain、boundary、requestId、attempt；复用原因码，不携带密钥或明文敏感输入。 | C03；O-004：旧异常到原因码完整表 |
| Core AckSelectionCandidate / NativeOfferBindingContext / NativeAdmittedOfferV3 / NativeSecurityPolicySnapshot | 持已有 Core 验证结果及绑定 request/attempt；admission 拥有不可变 candidate policy/public-key registry。verified offer 仅 admission 可构造，不接受独立 caller evidence DTO 或 policy 伪观测。ControllerVersion 仍由 Core 消息契约持有。 | CD-013；O-004：生命周期、generation fence 与真实回调签名 |
| NativeSealedPlan / NativeProviderGrantView / NativeGrantRequest / NativeGrantBinding | 来自 V12 与认证安全快照；角色、Provider、模型、request/attempt、期限、grantName 由一个 core 派生，不重复传入可覆盖字段。 | CD-003/004；O-004：按现有 signed-wire schema 逐字段冻结 |
| NativeCertifiedRecipe / NativeCanonicalSource / NativeCertifiedAssembly / NativeArtifactBinding | 只保存已认证 recipe/source/role binding、工件摘要与受控存储引用；装配成功才发布 certified assembly。 | CD-005/013；O-002：protobuf 原生精确字节；O-004：外部数据路径限制 |
| NativePreparedInput | 原生 encodeInput 后的 task/schema 身份、编码 bytes 或已认证引用；与 caller 的应用值分型，不能把裸 REPO_REF 标为 prepared。 | CD-013；V05/V06 来源映射 |
| NativeAssemblyControl | 单调 deadline、取消查询、generation fence 与资源上限；只在 owner 上推进，不能包含 Python helper 路径。 | CD-005 与 C14/M26--M28 |
| NativeConversationTurn / NativeCompletedAttempt / NativeConversationRecord / NativeConversationCheckpoint / NativeConversationContinuation | 分别表示待执行 turn、已验证执行结果、持久记录、原子提交点、下一轮只读续接引用；记录 generation、lineage、已提交 token 前缀及状态契约。 | CD-007；O-004：旧 journal 全字段、版本与崩溃恢复格式 |
| NativeServiceDefinition / NativeServiceRegistration | 前者拥有服务名和 native handler 配置，后者为 host 返回的不可伪造注册 token；关闭只影响本 host 注册。 | CD-014；O-004：实际 Core 注册/注销 API 与持有关系 |
| Existing native runtime DTOs | NativeSelectionProjectionV3、NativeProviderHandlerConfig、NativeCanonicalOnnxAssemblerOptions、NativeStandaloneTokenizerOptions 等按当前头文件 MODIFY/REUSE，不能建立平行副本。helper 配置删除及 decoderFactory 接线逐项见 symbol-design。 | O-004：删除字段的全部 caller 和配置迁移清单 |

## Readiness Rule

本表已覆盖上述 12 个来源类的所有声明字段，**不等于覆盖整个仓库全部 DTO 或完成 wire/ABI 冻结**。T001 必须按迁移入口递归展开依赖的 AdapterDescriptor、TensorContract、RoleAssemblySpec、资源/驻留、grant、安全和 journal 类型；每个叶子字段写类型、默认/必填、边界、owner、编码及测试 ID。未列的 nested type 或新增变量须先更新此表与覆盖清单。不得把 “沿用 Python 语义” 当作该项验收通过。
