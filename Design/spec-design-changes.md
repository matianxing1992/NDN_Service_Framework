# Spec 设计变更记录

## Spec190 B190-177 warm-path runtime validation — 2026-09-25

- **Status**: `PARTIAL`；B190-176 的受保护 plaintext/descriptor/session 复用已在真实
  normal Repo 两 Provider 三轮运行中验证，但 T011 完整性能资格仍未完成。
- **Evidence**: [B190-177](../specs/190-multiturn-latency/evidence/b190-177.md)。首轮为
  `CACHE_PLAINTEXT_MISS`，后两轮为 descriptor/template/plaintext/session hit；每个
  Provider `loads=1`、`hits=1,2`，后两轮 plaintext hit 到 session hit 约 `1.8–2.1ms`。
  C++ oracle、EOS、清理通过；phase timing 未启用，Repo restart 和完整 SC-001–009 仍未观测。
- **Design impact**: 无新的 API、wire、Repo、Selection 或授权变化；当前设计继续以源码事实
  描述 process-local plaintext lease、当前 grant 校验和 ORT session cache。当前/目标 PDF 不刷新。

## Spec190 T011 protected plaintext warm-path cache — 2026-09-25

- **Status**: `PARTIAL`；修复 Provider 在 ORT session hit 后仍重复完整密文校验和解密复制的
  原生准备路径；真实两节点 warm timing 尚未重新验收。
- **Owner / principles**: DI Provider/ONNX assembler 拥有进程级 plaintext lease cache；每个
  request 仍先完成当前 grant/Selection 校验，cache key 不含 request-local grant 字段，且包含
  受保护 key reference 和完整模型/图/role/recipe/backend identity。Repo、Selection、wire 和
  public API 不变，遵守 G1–G6、D1–D4。
- **Delta**: 新增私有 `NativeProtectedPlaintextCache`、move-only lease、descriptor reuse gate、
  `0600` plaintext staging 与 secure erase/stop/invalidate；后续同一 key 只复用 ONNX 路径，
  ORT session cache 继续由既有 owner 管理。无模型字节进入 cache heap；失效/停止仍等待
  active lease 后清理。
- **Evidence / boundary**: [B190-176](../specs/190-multiturn-latency/evidence/b190-176.md)。
  C++ production build、Provider assembly 22/22、actual protected `miss → hit`、staging
  cleanup、resident-session 12/12 和 Repo lookup 5/5 通过；新 normal Repo 两节点 warm
  timing、Repo restart、完整 Spec190 qualification 仍未验证，当前/目标 PDF 不刷新。

## Spec190 T007 Provider-owned resident CPU session owner — 2026-09-24

- **Status**: `PARTIAL`；实现了 DI Provider 内部的 bounded
  `OnnxRuntimeSessionCache`，并将不可变 ORT loaded session 与 fresh request
  wrapper 分离。只有显式 `residentSession=true`、CPU、完整 digest/contract
  identity 且非 protected/profiling backing 才可命中；原单参数 runner 入口和
  其他 profile 行为保持不变。
- **Owner / principles**: DI Provider/ONNX adapter 拥有 session lease、drain 和
  request-local evidence/KV 状态；Core/Selection/Repo/授权 wire 不变，遵守
  G1–G6、D1–D4。关闭时禁止 cold fallback，active lease 保证 shared session
  生命周期直到释放。
- **Delta**: 新增内部 `OnnxRuntimeSessionCache`、cache-enabled runner factory
  overload、Provider stop/drain 接线和 admission-time lazy idle sweep；修复无限 deadline waiter 不应使用
  `wait_until(time_point::max())` 的宿主相关自旋边界。未新增 public network API、
  模型字节传输或授权旁路。
- **Evidence / boundary**: [B190-49](../specs/190-multiturn-latency/evidence/b190-49.md)。
  native tiny-ORT 5/5、protected reuse 7/7、Provider assembly 21/21 通过；真实
  Qwen 两节点、TSan、授权失效和最终性能资格仍未验证，当前/目标 PDF 不刷新。

## Spec190 B190-32 partial publication repair — 2026-09-23

- **Status**: `PARTIAL`；增加原生内存态 `missingDataNames` repair hint，并在
  `ModelPreparationCache` 中要求 partial receipt 经过既有 Repo publisher 完成后才能保留。
- **Boundary**: hint 不进入 root manifest、持久身份、授权、Selection、Provider 或 wire；既有
  root-last/owned rollback/取消语义不变。完整命中仍可复用 reference-only material index；
  partial repair 不把该不完整 index 交给 publisher，而由 catalog 从 bounded source 重建完整
  manifest。直接 Repo repair 与 fresh source-child、material-payload `user.prepare()` 均已验证；
  layer 专门缺对象、完整计数和 protected reuse 仍未完成，因此 T005 保持 `PARTIAL`，T006 不解锁。
- **Evidence**: [B190-32](../specs/190-multiturn-latency/evidence/b190-32.md)。

## Spec190 current T006 static audit — 2026-09-23

- **Status**: `PARTIAL` / `NO_DESIGN_CHANGE`；B190-31 只审查当前实现与 CD-09/FR-016
  的一致性，没有修改公共 API、wire、加密算法或当前/目标 PDF。
- **Finding**: 当前 `lookupPrepared → ModelPreparationCache::buildPackage` 合同把单个缺失
  子对象折叠成完整 source miss，不能证明“缺一对象只补必要范围”。该缺口必须由原生
  Repo/DI owner 通过 B190-32 先闭合；不以已有 B190-30 hit 证据覆盖。
- **Boundary**: T006 仍需真实 current grant/Selection/placement、保护材料缺对象、Provider
  重启 serving、revoke/rebind 与 real route 证据；T007 不解锁。
- **Evidence**: [B190-31](../specs/190-multiturn-latency/evidence/b190-31.md)；当前审查快照
  及五 lane coverage matrix 已记录。

## Spec190 async prepare Repo lookup parity — 2026-09-23

- **Status**: `NO_DESIGN_CHANGE`；B190-30 修正 T005 公开异步入口与既有 CD-08
  契约的不一致，T006 仍 `PARTIAL`。
- **Delta**: `User::prepareAsync()` 现在与同步 `User::prepare()` 一样，在 source
  load/publication 前调用可选的 bounded `lookupPrepared`。命中仍要求当前 catalog
  和 receipt 校验；没有改变 publication identity、Repo wire、授权或 public API。
- **Boundary**: 当前命中后仍会做一次有界 source read 以重建 native catalog；这不是
  第二次 STORE/ingest，也不等同于 protected material/network zero-byte qualification。
  T006 的 current grant、Selection/placement、Provider restart 和 real route 仍未闭合。
- **Evidence**: [B190-30](../specs/190-multiturn-latency/evidence/b190-30.md)；affected
  C++ target compile/link passed，fresh-Runtime async selector 独立 3 次通过；当前/目标
  PDF 无语义变化，不刷新 PDF。

## Spec190 Face-backed exact protected grant fixture — 2026-09-23

- **Status**: `PARTIAL` / `NO_DESIGN_CHANGE`；B190-28 只修正 T006 C++ fixture
  的 Data wire 证据，不改变 NDNSF 外部契约或生产 NFD transport。
- **Delta**: grant publication now creates a signed exact-name Data on the
  Provider-owned Face, and the injected bounded fetch dependency obtains it
  through an exact non-prefix Interest. This validates the existing
  `ProtectedGrantFetcher` transport boundary rather than adding another
  provider path.
- **Boundary**: Core/Provider wire、Selection、grant binding、cache identity、
  authorization 和应用 API unchanged；real NFD/MiniNDN route、online
  Controller/revoke/restart qualification remain open. Current/target PDF no
  semantic change, so no PDF refresh.
- **Evidence**: [B190-28](../specs/190-multiturn-latency/evidence/b190-28.md)。

## Spec190 default protected grant factory transport seam — 2026-09-23

- **Status**: `PARTIAL` / `NO_DESIGN_CHANGE` for the external NDNSF contract；B190-27
  只推进 T006 的 C++ production-wiring sub-gate，不解除 protected material reuse
  安全门，也不解锁 T007。
- **Delta**: `NativeProviderHandlerConfig` 增加可选的 bounded、cancellation-aware
  `ProtectedGrantFetcher`。空值保持现有 exact-name NDN transport；Provider 的
  test-only owner seam 可以注入等价 fetch dependency，从而验证默认
  `installNativeProtectedGrantFactory`，不再用 `testProtectedRuntimeFactory` 替换它。
- **Boundary**: 不改变 Core/Provider wire、Selection、grant binding、cache identity、
  authorization 或应用公开 API；registry、recipient key 和 grant verifier 仍在生产
  factory 内 fail-closed。该 seam 不是 real NFD route-backed qualification；在线
  Controller、revoke/rebind、negative exact-fetch、durable key recovery 与完整 T006
  仍未完成。当前/目标 PDF 无语义变化，不刷新 PDF。
- **Evidence**: [B190-27](../specs/190-multiturn-latency/evidence/b190-27.md)；affected
  C++ target compile/link、focused selector 13/13 ×4 和 `spec185-provider-assembly`
  21/21 通过；full prepared-request regression 的独立 preparation timeout 不计 PASS。

## Spec190 native protected assembled ciphertext reuse — 2026-09-23

- **Status**: `PARTIAL`；B190-21 只闭合 native assembled cache 的稳定密文路径和
  Provider 当前进程解密，不把 T006 写成完整 protected reuse，也不解锁 T007。
- **Delta**: 普通 protected assembled entry 由 `recipeDigest`、role assembly-spec
  digest 和 opaque key-reference digest 寻址；持久目录只保留 authenticated ciphertext
  与 manifest，lookup 返回 descriptor，Provider 在当前 `ProtectedRuntime` 下建立新的
  request-scoped plaintext staging。内存 runner/artifact cache 仍按 grant identity 隔离。
  request ID、attempt、旧 grant、Provider boot 和 KV 不进入 durable assembled identity。
- **Boundary**: 这不是公共 API 或 wire schema 变更。C++ regression 已证明稳定路径命中、
  实际解密、错误 key-reference/AAD 拒绝和 independent-grant runner 隔离；尚未证明 OS
  restart/key-reference recovery、在线 Controller、grant/Selection/placement rebinding、
  revoke invalidation、精确 missing-object fetch、manifest tamper qualification 或真实
  Qwen/MiniNDN。
- **Evidence**: [B190-21](../specs/190-multiturn-latency/evidence/b190-21.md)；普通根
  `build/` 的 affected target 与 assembly worker compile/link，以及完整 21-case C++ target
  均通过。

## Spec190 Core-owned protected reference recovery — 2026-09-23

- **Status**: `PARTIAL`；B190-18 只推进 T006 的 Core durable reference recovery，不解除
  protected assembler miss，也不解锁 T007。
- **Delta**: `ServiceUser` 为稳定 publication identity 持久化非秘密 reference metadata，
  在 Repo durable lookup 后校验内容摘要、大小、policy/protection epoch、opaque key-reference、
  manifest、locator、owner 和首段可读性；命中后直接恢复已有 range serving，不重新加密或
  `commitFile`。reference 只有在 Repo commit/read-back、local owner registration 和 expiry
  registration 成功后才写入；缺失或损坏 reference fail-closed。
- **Boundary**: 该记录不持有 private key、plaintext、旧 grant、完整 request 或 session KV，
  也不改变 Repo 的 ciphertext-only owner。当前实现尚未证明真实 OS process restart、wrapped-key
  decrypt serving、grant/Selection/placement rebinding、revoke invalidation、Provider/assembled
  hit 或 Qwen/MiniNDN qualification；因此不把结构化 reference recovery 写成完整 protected
  reuse。
- **Evidence**: [B190-18](../specs/190-multiturn-latency/evidence/b190-18.md)；affected C++
  target build、8-case suite 和 restart/missing-reference selector 3 次通过。

## Spec190 single-supervisor MiniNDN startup and swap diagnostics — 2026-09-23

- **Status**: `PARTIAL`；真实 r16 仍在资源边界停止，不能视为 T003 或两 Provider 验收通过。
- **Delta**: LocalExperiment 作为唯一 host resource supervisor；Native MiniNDN runner 接收
  `--direct-start` 后直接创建 MiniNDN，消除内层重复 supervisor。`ownedSwap` 继续写入 raw
  resource samples 作为诊断，不再单独触发停止；`MemAvailable`、`SwapFree`、磁盘和 deadline
  仍是硬门。该调整只改变实验编排/资源观测，不放宽 grant、ACK、Selection、Repo、assembly、
  ORT、terminal 或 cleanup 契约。
- **Static boundary**: C++ `assembleInProcess` file-backed worker 使用
  `loadRuntimeSession=false`；Provider 父进程在 worker 完成后创建 authoritative ORT session，
  因此不新增 worker/runner 重叠层或 adapter。定向 Python 回归 `61 passed`；真实 r16 原始证据
  与未闭合边界见 [b190-03](../specs/190-multiturn-latency/evidence/b190-03.md)。
- **Installation correction**: r17 发现 root 使用的已安装 ONNX adapter 未包含当前
  `TensorProto.dims` canonical-identity 修复，产生 `SOURCE_IDENTITY_MISMATCH`；仅以
  `--no-deps` 重装现有 adapter，使安装模块与源码 hash 一致。该闭合 installation
  provenance，不改变模型/协议/授权契约；fresh r18 尚未运行。

## Spec190 direct-start simplification and Stage-0 ORT boundary — 2026-09-23

- **Status**: `PARTIAL`；r18 已越过 ACK/Selection 并进入 Provider-0 C++ ORT 执行，但在
  `/model/Gather_5` 发生输入索引越界，未形成 terminal/EOS/KV 验收。
- **Delta**: `--direct-start` 现在在当前 Native MiniNDN 进程直接进入 supervised 主路径，
  不递归重新解析命令，也不创建内部 worker/supervisor。LocalExperiment 仍是唯一 host
  resource supervisor；必要的模型身份、授权、ACK、Selection、Repo、assembly、ORT 和
  cleanup 约束保持不变。
- **Evidence**: r18 外层资源 guard 未触发、无残留进程；Provider-0 达到
  `MODEL_MATERIALIZED`、`WORKER_START`、`RUNNER_SPEC_READY`、`RUNNER_CREATE_BEGIN`，随后
  C++ ORT 报 `indices element out of data bounds, idx=1 ... [-1,0]`。下一 Changed gate
  限定为 Stage-0 prefill `attention_mask`/`position_ids`/shape 追踪，不增加 launcher 层。

## SIF Waf install payload — 2026-09-22

- **Status**: `NO_DESIGN_CHANGE`；脚本静态检查通过，不表示镜像构建或运行验收。
- **Scope**: 模板将根 Waf 的 DESTDIR 安装树作为原生文件唯一安装来源；
  第二轮以 `installed-v1` 收敛完整 SIF；测试附件改由 Waf 显式 opt-in 安装，
  外部依赖直接消费 base SDK，不再强制外置 APP 导出。Python 绑定仍在容器内构建。
  构建选项和 build-record 布局契约见对应脚本/审查，不涉及运行协议/API；遵守 B1–B3、S1–S3。
- **Owner**: 根 Waf 管安装清单，SIF 模板管搬运/核对，base 管外部依赖；
  不修改 Core/DI/UAV/Repo 协议、公开 API、当前/目标设计正文或 PDF。
- **Evidence**: [static review](../Experiments/TigerCluster/docs/waf-install-static-review-20260922.md)。
  静态/离线脚本检查不能替代 compile-link、ABI 或完整 SIF 运行验证。

## Spec185 User-owned request initiation — 2026-09-22

- **Status**: `PARTIAL`，见[API owner correction evidence](../specs/185-prepared-model-runtime/evidence/api-owner-correction-20260922.md)。
- **Delta**: 普通应用请求入口从语义上归属于 `User`：新增
  `User::request/run(const PreparedModel&, ...)` 与
  `User::openConversation(const PreparedModel&, ...)`。入口使用既有 package
  `runtimeBinding` 校验模型和 User 属于同一 Runtime，再复用原生
  `PreparedModel::requestInternal`、coordinator 和 lease；没有新增 Core wire、
  client、operation state machine 或 Provider 逻辑。旧 PreparedModel 入口暂保留
  为 compiler-deprecated compatibility wrapper。
- **Documentation**: Spec185 C++ contract、API catalog、caller matrix、quickstart、
  examples 和 request evidence 已改为 User-owned route；当前/目标 Design PDF
  尚未刷新，不能把文档改动写成完整设计交付。
- **Binding**: Python thin binding 已同步暴露 `User.request/run/open_conversation`；
  旧 `PreparedModel` binding 仍作为兼容入口保留。候选 DI 库优先加载下 Python
  定向套件 `10 passed`，但这不替代 C++ request selector 的 runtime 证明。
- **Validation boundary**: affected DI library 与 `spec189-prepared-request`
  target compile-link PASS；focused runtime selector 在既有 preparation/fixture
  边界 30 秒 `RC=124`，因此不改变 T005/T006/T013 的原验收结论，也不宣称本次
  API correction runtime PASS。

## Spec190 Multi-turn token latency — 2026-09-22

- **Serial revision**: 仅执行契约变更，无产品API变化；tasks改STRICT_SERIAL单链，旧T008/009/010/011/006/007分别为新T006/007/008/009/010/011。下述先前T011安全门现为T009；历史证据不改写，双PDF不因调度重排重建。

- **Status**: `PLANNED`，见[Spec190](../specs/190-multiturn-latency/spec.md)、[设计契约](../specs/190-multiturn-latency/contracts/design.md)与[任务](../specs/190-multiturn-latency/tasks.md)。
- **Owner / principles**: Core仍拥有通用ACK/认证/传输，DI拥有Conversation、FINALIZE控制和CPU session驻留；G1–G6、C1–C3、D1–D4、B1–B3、M1–M3不变。
- **Proposed delta**: Qwen profile ACK目标1000ms；原生实时token与同handle多轮；定位并修复约30秒FINALIZE收尾；loaded-session与请求证据/KV分离、有界租约和退出释放。
- **Scope revision**: 按用户补充纳入stage传输预算与每node固定持久Repo；`user.prepare(model)`命中prepared receipt免重复拆层/导出/STORE。Repo遵循R1–R6，Core继续拥有加密/授权；CD-09恢复接口冻结前T011 BLOCK，不能以compatibility替代真实Repo。见[材料复用契约](../specs/190-multiturn-latency/contracts/material-reuse.md)。
- **Boundary**: 本轮仅源码/已有raw分析及规划审计，无产品API/实现变化；当前/冻结目标PDF不自动覆盖。实施时随对应任务同步适用中文API与双PDF，不能把本Spec规划写成当前源码行为。Spec189保持PARTIAL。

## Spec189 Request-scoped epoch runner reuse — 2026-09-22

- **Status**: `PARTIAL`，见[r260](../specs/189-qwen-two-provider-minindn/evidence/b189-r260-runner-reuse.md)。
- **Owner / principles**: DI coordinator拥有一次request/role内延迟准备的runner；G1–G6、D1–D4。
- **Implemented delta**: 同一调用的token epochs与finalize共享runner，不跨request/role/attempt，
  保留每epoch授权、KV、取消与提交检查；不改wire或公共ABI，不改ACK/cleanup预算。
- **Boundary**: C++45 cases/1042 assertions三次通过；真实三轮相同32token、KV恢复、
  每Provider每轮一次准备、C++缓存诊断及cleanup通过。用户追加的
  短期跨request驻留另记PLANNED，需分离loaded-session与request证据/KV、显式evict及
  shutdown drain/release。API参考/双PDF未同步，不改冻结目标。

## Spec189 Conversation affinity and retention — 2026-09-22

- **Status**: `PARTIAL`，见[r259契约与验证](../specs/189-qwen-two-provider-minindn/evidence/b189-r259-affinity-retention.md)。
- **Owner / principles**: DI coordinator拥有本地认证映射，既有placement子类执行偏好，
  Provider拥有KV保留与receipt；遵守G1–G6、D1–D4、B1–B3、M1–M3，不下沉Core。
- **Delta**: 本地journal加密正文可选保存role→Provider映射并核对checkpoint摘要；
  续轮通过本地planning context传入既有策略。网络checkpoint/transcript不变，旧journal
  无偏好仍走原选后校验。Provider retention显式有界，receipt不得超过实际KV寿命；
  coordinator不再暗中把明确配置截为5分钟，不自动续期或复活过期状态。
- **Boundary**: 涉及DI内部结构/Journal函数ABI，受影响原生CLI已重编；C++定向回归
  三次通过、真实两Provider三轮缓存诊断及cleanup通过。binding fresh rebuild、当前API
  清单和双PDF仍待同步；中文契约见r259 Design binding，冻结目标不自动覆盖，正式资格未关闭。

## Spec189 Core assignment externalization order — 2026-09-21

- **Status**: `PARTIAL`。Core共享外置helper提前到首次envelope编码前，保留1MiB/4MiB
  门和原加密Data引用契约；完整选中集合先过授权/大小预检，失败按发布阶段清理或保留
  bounded serving，提交异常取消invocation。DI保留Core错误原因，不改公共签名或ABI。
- **Evidence**: [r257](../specs/189-qwen-two-provider-minindn/evidence/b189-r257-core-assignment-externalization.md)，
  Core八场景163 assertions三次PASS；真实双Provider10token/EOS缓存诊断PASS。
  API参考已同步行为/验证范围；PDF尚未刷新，不称完整文档交付或Repo产品验收完成。

## Spec189 bounded multi-token generation — 2026-09-21

- **Status**: `PARTIAL`。既有token预算上限64调整为1024；EOS/EOT优先，未引入字符限制、
  API字段或ABI布局变化。仍保留1MiB capability与4MiB Selection约束。
- **Boundary**: C++实现、C++行为oracle、Python仅编排；缓存诊断不计完整Repo资格。
  当前/目标PDF与公开API参考尚未同步，本轮不将文档交付记为完成。
- **Evidence**: [r256](../specs/189-qwen-two-provider-minindn/evidence/b189-r256-multitoken-stop.md)。

## Spec189 finalization result and feedback identity — 2026-09-21

- **Status**: `PARTIAL`。纠正既有 C++ coordinator 实现：成功完成 checkpoint
  finalization 不再报告 stoppedByUpstream；普通 STOP 不变。TOKEN_FEEDBACK
  从 accepted 配置补齐身份，非空冲突摘要拒绝；PIPELINE 受保护边不改。
- **Boundary**: 不改 API 签名、ABI、wire 或授权范围，不提前 Provider terminal。
  当前/目标公开接口和设计 PDF 无新增契约；仅分置两 Provider 为本次验证目标，
  不据此声称共置 placement 或全路径资格通过。
- **Evidence**: [r254](../specs/189-qwen-two-provider-minindn/evidence/b189-r254-finalize-feedback.md)。

## Spec189 atomic runtime evidence — 2026-09-21

- **Status**: `PARTIAL`。Provider 请求期机器记录先组装整行，再使用既有
  RuntimeTiming/ndn-cxx 后端；动态 CR/LF 归一化，防止多个 writer 交错破坏
  grant/attempt/plan 摘要。旧 stdout observer 同步解析实际日志流。
- **Design boundary**: 不改变公共 API 签名、wire、模型准备/执行或授权状态；
  不提前输出终态，也不降低 oracle 校验。当前/目标 API 和 PDF 无契约变更；
  本条仅记录内部诊断输出与编排 observer 迁移，不宣称模型资格完成。
- **Evidence**: [r252](../specs/189-qwen-two-provider-minindn/evidence/b189-r252-evidence-lines.md)。
  五 lane 静态复审通过，定向构建/测试进行中；独立 FINALIZE 收尾观测仍待处理。

## Spec189 cross-process cold-assembly admission — 2026-09-21

- **Status**: `PARTIAL` / internal native resource-admission behavior。Provider
  在 recipe-addressed cache miss 后进入 assembler cold path 前，通过共享
  `cold-assembly.lock` 和 advisory `flock` 串行化 model-sized working set；cache
  hit 不等待。等待循环保留 request cancellation、hard deadline 和 assembly
  timeout 检查，并输出 `COLD_ASSEMBLY_WAIT` / `COLD_ASSEMBLY_ENTERED`；RAII 保证
  异常、超时和正常 cleanup 释放锁。
- **Design boundary**: 生产 Provider 使用固定 artifact-cache root 下的共享锁；
  直接 assembler caller 未提供路径时从 cache root 父目录派生。该变化不改变
  cache recipe identity、model bytes、grant、ACK、Selection、placement、runner、
  KV、terminal 或 cross-Provider authorization；它只限制 cooperating NDNSF
  Provider 进程的冷启动并发。真实 MiniNDN run 尚未越过 `PROVIDER_READY` 后的
  request/assembly boundary，因此不能把资源改善写成 qualification PASS。
- **Source / evidence**: `NativeCanonicalOnnxAssembler.{hpp,cpp}`、`Provider.cpp`；
  affected build/install 和 focused selector 结果，以及 raw launcher `137` 边界见
  [Spec189 r244 evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-r244-cold-assembly-gate-20260921.md)。

## Spec189 recipe-addressed assembled-cache identity — 2026-09-21

- **Status**: `PARTIAL` / internal native cache behavior。assembled cache 的
  lookup key 从组装完成后的 `model.onnx` SHA 改为组装前已认证的
  `recipeDigest`；目录为 `assembled/<safe-role>/<recipeDigest>/model.onnx`。
  `manifest.json` 保存并校验 `assembledModelDigest` 以及 root/graph/initializer/role
  identity，命中前再计算文件 SHA。这样首次运行可按 model/layer/node/backend
  contract 查找，不扫描同 role 的未知版本，也不把结果摘要错误当作输入键。
- **Design boundary**: `recipeDigest` 继续包含 model manifest/root、canonical
  graph、role/node/layer set、tensor boundary、initializer、backend ABI、precision、
  layout、padding、assembler profile 和 limits。纯 ONNX bytes 的跨 CPU/GPU 复用
  仍可作为未来拆分的文件层优化；当前缓存交付 runner-compatible assembly contract，
  所以 backend identity 保留。authenticated Selection/grant、普通 protected Repo
  的 encrypted semantics、runner/KV/lease 生命周期和 wire contract 不变。
- **Source / evidence**: `NativeCanonicalOnnxAssembler.{hpp,cpp}`、
  `DI_NativeProviderExecutable.cpp`、`di-prepared-provider.t.cpp`；详见
  [Spec189 recipe-key evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-cache-recipe-key-20260921.md)。
  `di-native-canonical-publisher.t.cpp` 新增固定种子多层 ONNX 的准备/拆分/组装
  selector；`Spec185ProviderAssembly` `20/20`、canonical publisher `16/16`
  （397 assertions）、模型准备 case `1/1`、Python launcher/cache tests `29 passed`；
  真实 MiniNDN/Qwen two-provider 仍保持 `RUNTIME_UNQUALIFIED`，因此不关闭产品任务。

## Spec189 initial producer-readiness deadline boundary — 2026-09-20

- **Status**: `PARTIAL` / `NO_PUBLIC_API_CHANGE`。r140 暴露了一个生产时序边界：Provider-1 在 Provider-0 发布首个 placement-bound tensor manifest 前开始精确获取，但首个 manifest Interest 也被 `noProgressDeadlineMs` 截断。当前修复让 `NdnsfCollaborationDependencyIo::prefetchInput` 的首个 V3 manifest 等待使用 request hard deadline 与 dependency fetch budget；manifest 成功后，segment fetch 仍受原有 no-progress 与 fetch budget 约束。
- **Design boundary**: 不增加 hard deadline，不绕过 terminal/cancel，不改变 grant、ACK、Selection、`NDNSF_DATA_V1` wire 或 manifest 校验。变化只区分“producer 尚未发布首个 manifest”的 readiness wait 与“manifest 已发布后的 segment progress”；未宣称真实 Qwen/MiniNDN 或 qualification PASS。
- **Source / evidence**: `NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.cpp`；新增 C++ `V3DependencyIoWaitsForInitialProducerReadiness` 先以 `noProgress=100 ms` 重现旧实现失败，再在生产修复后通过；同一 V3 selector 的既有 manifest/segment 回归也通过。详见 [r141 initial-readiness evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-r141-initial-readiness-progress-20260920.md)。
- **Documentation boundary**: `prefetchInput` 的声明、参数和返回类型不变，因此本单元不生成新的 API signature/PDF 输入；当前行为、失败边界和下一次真实 run 要求由本条、Spec contract、tasks 与 evidence 同步记录。

## Spec189 protected source-staging cleanup boundary — 2026-09-20

- **Status**: `PARTIAL` / internal native resource-ownership repair。受保护的
  Post-Selection assembly 在 worker 已消费并校验 canonical source 后，先释放
  `canonical.onnx` staging 文件；该早期释放现在复用受保护目录的 pinned fd，
  对普通文件执行覆盖、`fsync`、`unlink`，再进入 ciphertext sealing 和
  request-scoped plaintext materialization；删除失败显式报错，不静默假设资源已
  回收。受保护
  artifact directory 以 runner-spec lifetime 转交 Provider cache，覆盖 assembler
  返回到 cache owner 安装之间的异常窗口；首个异常仍保留，zeroization/cleanup
  失败写入稳定诊断 marker。
- **Design boundary**: 只减少 protected assembly 的磁盘 working set；不改变
  source/initializer/graph/recipe digest、grant、ACK、Selection、placement、
  runner、terminal 或 ciphertext schema。非 protected plaintext 路径保持原有
  finalization cleanup。r139 首个生产边界仍是 host `diskFree`，该变更待受影响
  C++ selector、安装身份和新的 guarded MiniNDN run 验证。
- **Source / evidence**: `NativeCanonicalOnnxAssembler.cpp`、`Provider.cpp`、
  `ProviderArtifactCache.cpp`、`NativeModelRunner.hpp`；r139 raw run 和
  resource summary 记录于 Spec189 evidence/failure log。v2 read-only review
  报告了早期 `std::filesystem::remove` 绕过 secure erase 的 P1；当前修复把
  eraser 绑定到同一目录 lease fd。受影响 C++ selector、build/install identity
  和新 guarded MiniNDN run 仍待验证；此次变更不能把 r139 或 focused selector
  结果提升为产品资格 PASS。
- v3 read-only review 进一步发现 early eraser 与最终 lease drain 共享 fd
  但没有共享锁，以及 range-backed material 校验仍直接读取空 `data()`。
  当前修复为目录 lease 的早期擦除和最终 drain 增加同一互斥，并让模板/节点
  校验先生成 `copyBytes()`。v4 immutable snapshot 已获 `STATIC_PASS`；仍需
  focused selector、安装身份和 guarded run。

## Spec189 worker post-parse source release boundary — 2026-09-20

- **Status**: `PARTIAL` / internal worker working-set repair. After OA04
  `ownedSourceModel()` has parsed, validated, and copied the authenticated
  source into the child-owned ONNX model, the worker may scrub and release its
  request model/initializer buffers before continuing S4-S7. The in-process
  OA01 entry remains non-destructive.
- **Design boundary**: this changes only child-local plaintext lifetime and
  resident memory. It does not change S1-S7 checks, recipe/identity digests,
  ORT loading, worker framing, grant, ACK/Selection, placement, or terminal
  contracts. The release is after the source-consuming call returns; failure
  before that point retains normal exception cleanup.
- **Source / evidence**: `NativeOnnxRecipeAssembler.cpp` and
  `NativeOnnxAssemblyWorker.cpp`; r131 remains a
  `RESOURCE_BOUNDARY:ownedSwap` with no protocol or qualification result.
  The r132c affected build `538/538`, material selector `2/2`, and
  ONNX/Repo selector `30/30` passed; installed-runtime identity must still
  pass before the next guarded MiniNDN retry.

## Spec189 worker certified-chain move boundary — 2026-09-20

- **Status**: `PARTIAL` / internal native working-set repair。OA04 certified
  chain 在 S5 shape inference 前保留经过认证 node 的 deterministic bytes，
  然后 move authenticated `original` into `inferred`，不再深拷贝包含完整
  external initializer 的 protobuf model。
- **Design boundary**: 只减少 worker 内部重复 source/model allocation；S1–S7
  checks、recipe/node coverage、identity digest、ORT session、worker framing、
  grant/ACK/Selection/placement/terminal contract 不变。r129 仍在真实
  MiniNDN assembly 前被 host `MemAvailable` guard 停止，不能提升资格。
- **Source / evidence**: `NativeOnnxRecipeAssembler.cpp`；r130 targeted
  build `538/538`、material selector `2/2`、ONNX/Repo selector `30` cases
  are the focused gates. 下一步是安装受影响 worker/DI targets 并以新 run root
  做 guarded MiniNDN retry。

## Spec189 worker parent-source release boundary — 2026-09-20

- **Status**: `PARTIAL` / internal native API ownership adjustment。OA02
  worker transport 新增可选 `sourceToReleaseAfterWrite` 参数；生产
  `NativeCanonicalOnnxAssembler` 在请求帧完全写入 anonymous pipe 后 scrub
  并释放父 Provider 的 model/initializer/material source，既有默认调用保持
  non-destructive。该写入完成屏障之后子 worker 已拥有唯一仍需的请求副本。
- **Design boundary**: 只改变 assembly parent/child 的 resident working-set
  与明文生命周期，不改变 recipe digest、grant、ACK、Selection、placement、
  terminal 或 worker framing。若写入未完成，source 仍保持有效；失败路径继续
  由 worker transport 和 assembler cleanup 负责回收。r121 的宿主资源边界尚未
  通过，不能提升 Spec189 资格。
- **Source / evidence**: `NativeOnnxAssemblyWorker.{hpp,cpp}`、
  `NativeCanonicalOnnxAssembler.cpp`；详见 [r121 resource evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-r119-range-source-focused-check-20260920.md)。
  下一步是 affected C++ selectors、installed identity check 和全新 guarded
  MiniNDN retry。

## Spec189 native runner/artifact cleanup boundary — 2026-09-20

- **Status**: `PARTIAL` / internal native API change。`NativeModelRunnerSpec` 新增
  `std::shared_ptr<const void> lifetime`，由活动 runner 持有请求/缓存材料；cache
  publication 会清除 metadata template 的 owner，避免缓存条目把自己的 lease 永久
  固定。`NativeCanonicalOnnxAssembler` 暴露进程内
  `withNativeArtifactDirectoryFinalization(directory, action)`，Provider owner cleanup
  与 assembler finalization 共享同一收尾锁，并在 canonical physical path 下执行。
- **Design boundary**: 该修复只约束 native artifact 的所有权、淘汰和同进程目录收尾，
  不改变 grant、ACK、Selection、placement、Repo wire 或模型输出协议。跨独立进程共享
  cache directory 仍未提供锁，不能宣称跨进程并发安全。
- **Source / evidence**: `NativeModelRunner.hpp`、`Provider.cpp`、
  `ProviderArtifactCache.cpp`、`NativeCanonicalOnnxAssembler.{hpp,cpp}`；详见
  [B189 r8/r91 evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-memory-lifecycle-r8-and-r91-20260920.md)。
  冻结范围官方 `review-agent` 返回 `STATIC_PASS`，DI target closure 与 focused C++
  selectors 通过；真实 Qwen r91 在两个 Provider ready 后因宿主 disk-free guard 停止，
  因此仍为 `PARTIAL`。
- **Documentation boundary**: API Markdown/JSON/PDF 的完整生成必须在干净文档
  checkpoint 重新运行；本条不能把工作树中的旧 API snapshot 视为已同步。

## Spec189 native memory release boundary — 2026-09-20

- **Status**: `PARTIAL` / internal API ownership adjustment。prepare publication 在
  material manifest 已认证后以不可变 `shared_ptr` 快照替换 full source；Post-Selection
  role model 物化后释放 selected material payloads/manifest。`sourceRefFor` 返回 owning
  immutable view，避免 catalog 原地清空造成并发读者失效。
- **Design boundary**: 这只改变 native working-set 和 source ownership，不改变模型摘要、
  grant、ACK、Selection、placement 或 terminal 协议；必须保留 PreparedModel/provider
  lease、ORT session/conversation state 和 protected Repo lease 到终态。
- **Source / evidence**: `NativeCanonicalPreparationCatalog.{hpp,cpp}`、
  `NativeCanonicalOnnxAssembler.cpp`、`ModelPreparationCache.cpp`、`Runtime.cpp`；详见
  [memory lifecycle audit](../specs/189-qwen-two-provider-minindn/evidence/b189-memory-lifecycle-static-20260920.md)。
  DI/selector compile-link 与两个定向 C++ selector 通过；冻结范围的官方
  `review-agent` 复审为 `STATIC_PASS / TESTS_DEFERRED`。完整 selector 仍有既有
  cancellation/staging fixture 失败，不提升 Spec189 产品验收。

## Object handles and replica locators — 2026-09-19

- **Status**: DOCUMENTATION_ONLY / NO_PUBLIC_API_CHANGE。用户接受 R6；适用 G3–G5、R1–R6、D2–D3。Repo 管理引用与副本定位，网络部署提供可达路由，DI 保持材料身份及执行范围。
- **Boundary**: handle 不授予权限、不延长 lease、不默认包含明文密钥；位置迁移不改变对象摘要。具体序列化、签名者规则、位置刷新和真实 NFD 取数尚待下层设计与验收；未更改源码、API 清单或冻结 PDF。
- **Validation**: 高层文档链接和限定 diff 检查；没有构建或运行实验。下一步定义 handle/位置记录契约及源离线、副本切换、过期位置的验收。

## Repo modes and DI ownership — 2026-09-19

- **Status**: DOCUMENTATION_ONLY / NO_PUBLIC_API_CHANGE；用户接受两模式方向，新增高层 R5，适用 G1–G6、R1–R5、D1–D4。
- **Owner**: Repo 管理存储、模式准入与读取生命周期；DI 管理准备材料、placement、缓存和 KV 语义。应用主动获取后本地缓存不视为远端 INSERT；显式 server 归档不视为 in-app 自动复制。
- **Evidence**: [两模式审查](repo-two-mode-analysis-20260919.md)，本轮核对 PreparedModel.cpp 的 repository input 和 ModelPreparationCache.cpp 的持有/退役 lease。高层链接、diff 与模式/DI 边界检查通过；没有运行测试或提升 Spec189 资格。
- **Remaining**: 统一写入/恢复/修复入口门控及服务路径验收待后续实施；当前 API/PDF 和冻结目标不因高层原则调整而改写为已实现。

## Reusable base and complete runtime SIF — 2026-09-19

- **Status**: DOCUMENTATION_ONLY / NO_PUBLIC_API_CHANGE。用户要求将可复用 base 与后续 NDNSF 更新流程纳入 [高层设计 S1–S3](highlevel-design.md#sif-构建与复用约束)，采用已声明外部依赖/SDK base 加容器内构建的 NDNSF，输出新的完整 SIF。
- **Boundary**: 不原地修改 base；依赖/工具链/ABI 变化重新验证 base，普通源码更新复用依赖。最终运行不依赖外置应用包或相邻 base；沿用维护入口和 Apptainer 1.5.3。本轮未改源码/API/冻结目标 PDF、未启动构建或实验。

## Build/install and MiniNDN constraints — 2026-09-19

- **Status**: DOCUMENTATION_ONLY / NO_PUBLIC_API_CHANGE。用户新增 [B1–B3、M1–M3](highlevel-design.md#编译安装与-minindn-实验约束)，明确 NDNSF 根 Waf 与外部依赖构建系统的责任，以及所有 MiniNDN 运行的系统安装边界。
- **Impact**: 使用构建树被测程序的现有启动器必须迁移后才能满足新约束；本轮仅更新文档，不启动构建、安装或实验。原始记录保留，运行目录仍可保存配置和数据。

## High-level design baseline — 2026-09-19

- **Status**: DOCUMENTATION_ONLY / NO_PUBLIC_API_CHANGE。按用户要求提炼 [highlevel-design.md](highlevel-design.md)，为四模块确立可引用的 G1–G6 与模块原则；当前摘要与必须保持的约束分开。
- **Scope**: README、MANAGEMENT 和架构阅读入口增加原则检查。保留现有未验收项和冻结目标，不将建议修复写为当前已实现；当前/目标 PDF 输入未变，本轮不重建。
- **Validation**: 四模块各 500–1000 字，链接及限定 diff 检查通过；后续代码修改须说明适用原则和证据。本条是文档治理交付，不关闭 Spec189 的任何产品任务。

## Spec189 authenticated assembly progress handoff — 2026-09-19

- **Status**: `PARTIAL`。B189-3 为 post-Selection assembly 增加了生产 C++ 进度回报和精确消费绑定：`NativeCanonicalOnnxAssemblerOptions::reportProgress` 在已认证里程碑回调，`StreamEventConsumer::observeAuthenticatedProgress` 只接受同一 request/provider/service、Selection digest、terminal-role operation、严格 epoch/sequence 和有效期限；`ServiceUser::initializeStreamConsumer` 传递 expected operation ID。
- **设计原因**：r53 的真实运行已到 grant verification 和 assembly staging，但 requester 因 stream event gap 超时，日志没有可验证的中间阶段。generic heartbeat 不能证明 assembly；进度必须由生产 assembler 的真实 root/material/source/worker 里程碑产生，并由 requester 绑定到本次 Selection。
- **当前/目标边界**：本地 C++ lifecycle、same-provider multi-role streamed fixture、D2b 和 worker-backed assembly selectors 已通过；初次 fixture 契约错误已由只读复审后的 test-only metadata 修复消除，完整 assembly suite 现为 9/9。真实 Qwen/MiniNDN、terminal/output/drain 和跨 service 负例未完成。不得将本条提升为资格 PASS。
- **源码与证据**：`ndn-service-framework/InvocationStream.{hpp,cpp}`、`ServiceUser.{hpp,cpp}`、`NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.{hpp,cpp}`、`Provider.cpp`、`examples/DI_NativeProviderExecutable.cpp`；详见 [B189-R4 evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-r4-progress-heartbeat-20260919.md) 和 v6 review snapshot。
- **文档同步边界**：本条登记当前源码行为和剩余验证；API Markdown/PDF 的完整生成与源码摘要刷新仍须在同一干净文档 checkpoint 按 `Design/MANAGEMENT.md` 完成，不能把工作树中的旧 API reference 当作已同步。

## Spec189 pre-root assembly admission correction — 2026-09-19

- **Status**: `PARTIAL`。r56 的真实候选已到 ACK/Selection 和 `BEFORE_ASSEMBLY` grant verification，但 requester 在 Provider 产生可观察 assembly 事件前因 stream gap 终止；这不是 ORT、Repo 或模型 PASS/FAIL 结论。
- **Design change**：将第一个 authenticated `ASSEMBLY_STARTED` milestone 前移到 root-material fetch 之前，覆盖 post-grant 的静默 admission 窗口。该变化不新增授权决策、不提高 timeout，也不改变 Selection digest/operation binding。
- **Evidence**：冻结修复快照获官方只读 `STATIC_PASS`；受影响 C++ closure 以 Waf `-j3` 重建，`Spec175InvocationStreamLifecycle` 15/15、`Spec175NativeAssembly` 9/9 通过。真实重跑及 terminal/output/drain 仍未完成，详见 [r56 evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-real-qwen-r56-20260919.md)。
- **Documentation boundary**：B189-3 的下一个且唯一优先出口是用重建候选做一次真实 retry；在观察新的第一生产边界前，不扩张组件职责、不盲目增加 timeout，也不把本地回归提升为资格 PASS。

## Spec189 provider-specific collaboration progress binding — 2026-09-19

- **Status**: `PARTIAL`。r70 真实运行已到 authenticated assembly admission 和 selected-material fetch，但 terminal stream consumer 在非 terminal Provider 继续组装时仍因 stream gap 终止。
- **Design change**: streamed collaboration progress now binds exact `{providerName, providerSelectionDigest, operationId}` tuples. Selection digests are Provider-specific because each key envelope is recipient-bound; freshness `(epoch, sequence)` is tracked per tuple so worker-to-terminal progress handoff is accepted while stale duplicates remain rejected. Legacy single-Provider consumers retain their exact operation binding.
- **Evidence**: r70 boundary is recorded in [native-r70 evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-native-r70-progressed-stream-boundary-20260919.md). The repair requires the reviewed C++ selector and a new candidate run; no qualification PASS is claimed.
- **Documentation boundary**: this entry records the current repair contract and its open validation. Generated API Markdown/PDF remains a separate MANAGEMENT checkpoint and must not be inferred from this working-tree entry.

## Spec189 Selection-scoped admission sequence — 2026-09-19

- **Status**: `PARTIAL` / `NO_PUBLIC_API_CHANGE`。为修复 assembler/rebuild 重建时重复从 sequence 2 开始的问题，`NativeSelectionProjectionV3` 现在携带不参与 JSON/canonical digest 的 runtime-only shared atomic counter；同一 authenticated Selection 的 admission 与每次 runner factory copy 共享它。
- **Design change**：`GRANT_VERIFIED` 后先报告 epoch 1、sequence 1 的 `ASSEMBLY_ADMISSION`，随后 assembler milestones 使用同一 counter 产生 sequence 2、3……。这保持 operation identity、授权和 Selection wire 不变，只闭合 Core 严格非零/单调序列约束。
- **Evidence**：v4 冻结快照获官方只读 `STATIC_PASS`；受影响 closure 真实编译链接成功；C++ lifecycle 1→2→3 与 `Spec175NativeAssembly` 9/9 通过。r58 在 ACK/Selection 前触发 `RESOURCE_BOUNDARY:diskFree`，因此没有真实 admission/assembly 结论。见 [admission sequence/r58 evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-admission-sequence-r58-20260919.md)。
- **Documentation boundary**：该内部状态不构成 Qwen、MiniNDN、ORT、output 或 reuse PASS；T006/T007/T009 继续 `PARTIAL`/blocked。

## Spec189 local MiniNDN runtime boundary — 2026-09-19

- **Status**: `NO_DESIGN_CHANGE`。本机 Spec189 验收直接使用宿主机已安装的全局 NDNSF/Core/Repo/DI 依赖和本机构建的 C++ APP；SIF/Apptainer/TigerCluster 不属于本地 MiniNDN 前置条件。
- **Evidence boundary**：r58 的 `RESOURCE_BOUNDARY:diskFree` 只表示宿主机工作区/运行时文件系统低于安全门；本轮没有启动或读取 SIF，也不能把该边界归因于 SIF 构建或容器内容。
- **Delivery boundary**：SIF/Tiger 保留给后续独立交付和远端资格，不改变 Spec189 的本地 C++/MiniNDN 完成门。

## Spec189 causal evidence checker — 2026-09-19

- **Status**: NO_PUBLIC_API_CHANGE / PARTIAL。仅增加 Provider 内部日志 `preparationId`（request/role owner 下每次 runner preparation 调用身份），不改变公开签名、授权、模型数据、operation status sequence 或 wire。
- **Evidence**: [causal oracle evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-causal-oracle-20260919.md)。C++ checker 按 ID 配对多 epoch 的 assembly/ready，并让正常 CLI 必经材料与末段终态门；这不是模型正确性或 MiniNDN 资格。
- **Documentation boundary**: 不将日志判据视为新目标 API，不覆盖冻结当前/目标 PDF；完整 Spec189 交付仍按原设计同步门验收。

## Spec189 DI/Repo repair analysis — 2026-09-19

- **Status**: NO_DESIGN_CHANGE。本轮仅评估静态审查问题的修复选项，建议为 PROPOSED，未修改产品源码、公开 API 或冻结目标。
- **Evidence**: [repair design analysis](../specs/189-qwen-two-provider-minindn/evidence/di-repo-repair-design-analysis-20260919.md)。已区分新 material-only consumer 的局部验证与真实生产链资格，未将旧的“尚未接入”结论延续为当前事实。
- **Next**: 用户接受边界后再形成实施契约；本轮不重新生成当前/目标 PDF，不宣称设计同步或产品验收完成。

## Spec189 execution and ownership audit — 2026-09-18 14:50 -0500

- **Status**: PARTIAL；本次仅修正 [Spec189 任务与批次](../specs/189-qwen-two-provider-minindn/batch-execution.md)，保留 7 个能力任务，T003 分 protected storage/atomic preparation 两个验证出口。
- **Target clarification**: Core 保留 protected serving、Repo 提供身份绑定范围存储；publication lease 服从 preparation cache 预算/淘汰，避免 publisher 第二强 owner。重型 commit 不阻塞 Core I/O，取消/drain 与同名替代 fence 为 B189-1a 必要出口。
- **Validation**: 请求 active owner 与 idle cache 分账；host guard 前置，native counters 随实际组件交付，真实重复验收前齐备。详见 [审计证据](../specs/189-qwen-two-provider-minindn/evidence/spec189-static-audit-20260918.md#execution-and-ownership-follow-up)。
- **Current boundary**: 已有 protected-store 源码草稿 STATIC_FAIL，当前/目标 PDF 与 API 不在本审计中宣称已同步或通过；B189-1a 实现交付须按 MANAGEMENT.md 同步，T009 最终文档门保留。本轮没有修改产品源码、构建或运行模型。

本文件回答“哪个 Spec，为什么，把哪个模块的什么设计从什么改成了什么，代码实现到哪里”。
这里记录设计影响；Spec 的 tasks.md 与契约继续负责具体任务和验收。CHANGELOG.md 记录文档版本，两者互相引用。

## 记录规则

1. 从本基线起，每个新 Spec 必须登记一条；旧 Spec 后续继续修改设计时也登记。没有设计变化写“无设计变化”并说明依据，不留空白让读者猜测。
2. Spec 建立时记录目标与受影响章节；目标调整时补充前后差异；实施和验收时分别更新实现状态与证据。状态使用 PLANNED、PARTIAL、VERIFIED、NO_DESIGN_CHANGE，不以 Spec 编号或代码提交代替验收。
3. 当前设计只写已核对的实际行为；未实现的目标留在目标设计。部分实现分别列出已接通和未接通部分。
4. 每条保存 Spec 路径、任务/契约 ID、模块、章节标题、变更前后行为、源码提交、文档提交定位和证据。章节标题为主要定位，页码仅辅助。
5. 源码提交使用完整哈希或明确范围；工作树快照必须附逐文件摘要和补丁。文档提交由 `git log -- Design/spec-design-changes.md` 定位，避免在提交内填写自身哈希。
6. 更新两份 PDF、正文、记录和必要的 Spec 进度，同一文档单元核对后提交。回退或取代某项设计时追加记录，不删除历史条目。

## 索引

### 2026-09-18 — Spec189 architecture and progress correction

- **Status: PLANNED / PARTIAL**。本轮只修正 [Spec189](../specs/189-qwen-two-provider-minindn/plan.md) 的目标边界与任务，不修改生产代码或公开 API。prepare 从固定两段最终模型改为拓扑无关原子层/shared 材料与 Repo 可达性；ACK 后规划、Selection 后范围物化。复用现有 Runtime/PreparedModel/Repo，不新增 Qwen API。
- **Current evidence**：基线 `76b26e2c` 加已有工作区实现；Repo 层 payload 与 PreparedModel 复用 selector 是组件证据，实际 requester/assembler 接线未闭合。r25 已到执行/组装入口，未通过完整模型运行。详见 [audit correction](../specs/189-qwen-two-provider-minindn/evidence/spec189-static-audit-20260918.md#architecture-and-progress-correction)。
- **Tasks**：旧 T002/T004 合并 T003、T010 合并 T009；T008 资源门前移。当前/目标 PDF 的冻结 API/源码快照本轮不覆盖；本条登记的是 Spec 内部目标修正，不宣称 PDF 已包含未实现设计。后续 T003/T006 实现改变 API/行为时按 MANAGEMENT 同步中文契约、API 参考与双 PDF，T009 文档交付检查不得跳过。
- **Validation boundary**：仅文档一致性与只读审查，无 native build、模型运行或 MiniNDN PASS；纯文档 checkpoint 不使未变二进制和模型证据失效。

### 2026-09-17 — Spec188 Model Preparation and Disk-Backed Artifact Memory Control

- **2026-09-17 progress gate reset / PARTIAL**：进度审计把本地产品出口进一步限定为 T005、T006、T008、T009、T010、T011；T014–T016 只在 native core 通过后作为 delivery gate，T002/T003/T004/T007/T012/T013 继续作为 follow-up。当前 `RepoSourceProvider::load()` 的完整 vector 组装、缺少默认 owner 注入和 current-candidate zero-republication 仍是开放证据缺口；没有新增 API、生产机制或 PASS。详见 [progress audit r3](../specs/188-model-preparation-disk-backed-memory/evidence/progress-audit-20260917-r3.md)。

- **PLANNED / NDNSF-DI + NDNSF-Repo**：依据当前源码和 Spec185/187 evidence，登记模型制品从 request-time publication 改为 `Runtime::prepare(ModelRef)` 阶段的目标边界。当前 `NativeCanonicalPreparationCatalog`/`NativeCanonicalArtifactPublisher` 仍可能在请求准备中持有和发布完整 source/initializer；`RepoCore`/`RepoNode`/`RepoClient`/`TieredRepoStore` 仍有 vector、BLOB、concat 和 cache copy 路径。目标增加 typed `ArtifactReference`、file-backed/range Repo、bounded hot windows、lease/pin 计量和 authenticated Selection 后 Provider assembly。
- **Before/after**：before，模型 bytes 可在 request-time publisher、encrypted envelope、NDN segment/IMS、Repo `StoredObject::payload`、runtime executor 和 Provider runner 多处同时存活；after（target only），prepare 幂等落盘并返回 reference/lease，request 不再携带或重复发布模型，Repo/Provider 只按范围读取并分别计量 immutable artifact、transfer、materialization、runner 和 input 内存。应用输入 `Input::repository(DataRef)` 保持独立，不被改成模型传输接口。
- **Contracts/tasks**：[Spec188](../specs/188-model-preparation-disk-backed-memory/spec.md)、[model reference](../specs/188-model-preparation-disk-backed-memory/contracts/model-reference.md)、[file-backed Repo](../specs/188-model-preparation-disk-backed-memory/contracts/file-backed-repo.md)、[memory budget](../specs/188-model-preparation-disk-backed-memory/contracts/memory-budget.md)、[tasks](../specs/188-model-preparation-disk-backed-memory/tasks.md)。Spec 状态为 `IN_PROGRESS`；T005 当前为 `PARTIAL`，已接入 `RuntimeConfig::repositorySourceProvider` 与 RepoCore-backed `RepoSourceProvider`，但 provider 仍返回完整 `NativeCanonicalSource` vector，lease 计量、当前 caller 注入和完整请求链仍未闭合。B188-1 r7 发现的 erase 顺序和 physical-usage recovery 缺陷已修复，并在 r8/r9 任务及组合静态门和 C++ selector 中通过；T002/T003 仍为 `PARTIAL`，扩展故障和 TSan 未观测。
- **Source/evidence**：当前事实来自 `NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalPreparationCatalog.cpp`、`NativeCanonicalArtifactPublisher.cpp`、`NativeRequestPreparation.cpp`、`Runtime.cpp`、`Runtime.hpp`、`ServiceUser.cpp`、`NDNSFMessages.cpp` 以及 `NDNSF-DistributedRepo` 的 RepoCore/Node/Client/TieredStore。当前 provider 的 C++ selector、matching host dependency closure 和复审记录见 [B188-3 current evidence](../specs/188-model-preparation-disk-backed-memory/evidence/b188-preparation.md#current-t005-provider-implementation-and-static-gate--2026-09-17)；r14 YOLO publication 行属于旧候选，当前 provider-bound YOLO 尚未重跑。API 清单已用 `build-api-reference.py --changed-only` 生成，但当前 source snapshot/PDF provenance 因并行工作树漂移保持未验证，不能宣称文档门禁通过。
- **Scope correction / PARTIAL**：本轮审计确认核心目标仍是 prepare→reference request→authenticated assembly/drain→YOLO 两轮本地链路；Qwen 完整推理、1.5 GB OS 级压力、TSan/parser-fuzz、远端/跨进程故障和 broad mutation matrix 从核心完成门移为 `FOLLOW_UP`/`UNOBSERVED`。保留原任务编号、历史证据和失败边界，不改变当前/目标设计快照，也不把任何未执行验证写成 PASS。详见 [scope audit](../specs/188-model-preparation-disk-backed-memory/evidence/scope-audit-20260917.md)。
- **Progress/code-reality correction / PARTIAL**：复核确认 RepoCore callback 只属于测试注入边界，`PreparedModelPackage` 仍通过 `NativeCanonicalPreparationCatalog` 保留 source，且 YOLO 两轮仍记录 `LARGE_DATA_PUBLISH_*`；下一批只推进真实 Repo owner/reference/lease 与 zero republication。Provider/Memory/YOLO selector 已按实际 Waf target/suite 修正，未新增 API 或核心机制。详见 [progress audit](../specs/188-model-preparation-disk-backed-memory/evidence/progress-audit-20260917.md)。
- **2026-09-17 r7/r8/r9 Repo repair and validation**：B188-1 r7 发现 manifest-first erase 与 physical-usage recovery 缺陷；当前已修复并由 r8/r9 冻结快照的任务及组合 `review-agent` 复审为 `STATIC_PASS`。r9 canonical affected closure 和三个 Repo C++ selector 在 ASan/UBSan 下通过；真实 ENOSPC/short-write/cancel、erase/fsync ambiguous fault 与 TSan 仍未观测，T002/T003 继续为 `PARTIAL`，不得将 focused PASS 写成产品完成。

- **2026-09-17 T005 provider wiring / PARTIAL**：当前工作区把 prepare-time Repo owner 从测试 callback
  扩展为 `RuntimeConfig::repositorySourceProvider` 与 RepoCore-backed `RepoSourceProvider`。
  Before，Runtime 只能通过兼容 `RepositorySourceLoader` 或本地 file 取得 source；after，首选 provider
  以 typed presence、manifest-first bounded range read/write、一次 miss ingest、digest/size/deadline
  校验接入 `Runtime::prepare`，并在带 receipt 的 publication 后释放 catalog transient source。
  旧 loader 与无 receipt source owner 保留为兼容边界。此为实际 API/ownership 变化，已完成 v4
  `review-agent` `STATIC_PASS`；随后 canonical ASan/UBSan `spec185-runtime` build exit `0`，
  provider miss-ingest/hot-hit、typed lifecycle-error 和完整 Runtime selectors 分别为 1/1、1/1、11/11。
  lease 计量、真实 request/YOLO zero-republication 和完整动态故障矩阵仍未验收；Spec188/T005 保持
  `PARTIAL`。受影响契约见
  [model reference](../specs/188-model-preparation-disk-backed-memory/contracts/model-reference.md)。

- **2026-09-18 T005/T011 code-reality reclassification / PARTIAL**：复核确认 `RepoSourceProvider::load()`
  虽使用 bounded range I/O，仍将块组装为完整 `NativeCanonicalSource` vector；因此不能把 range
  selector 当作 prepare 内存上界证明。当前 `Runtime::open` 和 `spec188-yolo-repeat` 也没有默认注入
  Repo owner，r14 的 `LARGE_DATA_PUBLISH_*` 只能作为旧候选历史证据。该条只修正当前/历史文档分界，
  不改变 API 或生产代码；下一步是 provider-bound current-candidate selector、lease/copy counter
  和两轮 YOLO zero-republication 验证。

- **2026-09-17 scope reset / PARTIAL**：再次审计确认 Spec188 的最短产品目标是当前
  `User::prepare(modelKey, PrepareOptions)` → Repo owner/receipt/lease → reference-only request
  → authenticated Provider assembly/drain → 两轮当前候选 YOLO。原先把通用
  `RepoCore`/`RepoNode`/`RepoClient` 大对象重构、完整 segmented serving、8 路 prepare、GB 级
  RSS/swap、Qwen 数值推理、扩展故障和 SIF/Tiger 放在同一完成门，已改为 `FOLLOW_UP`/外部门；
  T002/T003/T004/T007 的 focused 结果保留为复用基础。`prepare(ModelRef)` 修正为当前真实公开
  入口，不新增第二套 API。此为 Spec 范围和契约文档修正，没有把任何运行结果提升为 PASS；证据见
  [scope audit r2](../specs/188-model-preparation-disk-backed-memory/evidence/scope-audit-20260917-r2.md)。

- **2026-09-18 scope-convergence review / PARTIAL**：在 T005–T011 和 T014 的 bounded local
  exits 已有证据后，Spec188 的活动计划收敛为 Core、Delivery、Follow-up 三层，删除重复的
  scope-reset 叙述，保留历史 evidence 和 task ID 以便追踪。当前 `RepoSourceProvider::load()`
  仍把 range 块组装成完整 `NativeCanonicalSource`，所以本 Spec 只声明 receipt/lease/owner、
  reference-only request、authenticated assembly/drain 和本机 YOLO zero republication；不声明
  GB 级内存上界。T015 使用 Spec188-scoped source/API/profile manifest；全仓库 Design/source
  baseline/PDF 若受其他并行 Spec 影响，记录为 `UNOBSERVED`，不吸收无关改动。当前/目标设计继续
  分离，T015 的 scoped convergence 和 T016 本地 handoff 已完成，SIF/Tiger 保持外部边界与
  `WAITING_EXTERNAL_INPUT`。详见 Spec188 的
  [plan](../specs/188-model-preparation-disk-backed-memory/plan.md)、[tasks](../specs/188-model-preparation-disk-backed-memory/tasks.md)
  和 [convergence evidence](../specs/188-model-preparation-disk-backed-memory/evidence/b188-convergence.md)。

### 2026-09-15 — Spec187 Two-layer Packaging

- **NO_DESIGN_CHANGE / product APIs; PARTIAL / packaging**：用户确认 `base SIF + NDNSF`，外部依赖和 SDK 在现有 base 上增建，本仓库各模块归统一 NDNSF 层。此次改动仅构建脚本、技能与交付归属，不改变 C++/Python 产品 API、请求协议或对象生命周期，因此不刷新产品 API/PDF 快照。实现和验收见 [two-layer delivery](../Experiments/TigerCluster/docs/two-layer-delivery.md)、[Spec187 tasks](../specs/187-yolo-minindn-sif-app/tasks.md) 和 [candidate evidence](../specs/187-yolo-minindn-sif-app/evidence/b187-complete-candidate.md)；base SDK 已真实验收并封存，NDNSF candidate 仍保持 PARTIAL。

### 2026-09-12 — Spec185 Prepared Model Runtime

- **2026-09-12 21:07 -05:00 / T003+T004 B2 `VERIFIED`**: C-02 preparation design is now implemented for the bounded local Runtime path. Before, `User::prepare` had no verified Package/cache implementation; after, `ModelPreparationCache` owns canonical source inspection, independent graph identity, immutable `PreparedModelPackage`, single-flight/refresh generations, waiter cancellation/deadlines, leases, LRU/byte budget and exactly-once completion. Core `OperationRuntime` remains the generic owner and has no DI dependency. Source range is the frozen B2 snapshot from base `9bdde3cf` with tracked patch SHA `2d1f7772efef5a0cdc689e340599a2b752262abb673de698427eda3b52ff7d9a`; bounded C++ normal/TSan evidence is recorded in [B2 evidence](../specs/185-prepared-model-runtime/evidence/b2-preparation.md). Request, conversation, Provider, Python and cross-process qualification remain `PLANNED`.

- **2026-09-12 17:00 -05:00 / NO_DESIGN_CHANGE**: 逐任务静态门改为按依赖阻塞，允许单主会话编码与独立快照审查重叠；不改产品API、任务依赖或验收，双PDF无需重建。[验证](../specs/185-prepared-model-runtime/evidence/dependency-scoped-dispatch-20260912.md)。

- **2026-09-12 16:24 -05:00 / NO_DESIGN_CHANGE**: tasks Updated及新checkpoint要求分钟与UTC offset；仅进度元数据格式修订，不改API/行为/状态或双PDF。[验证](../specs/185-prepared-model-runtime/evidence/progress-timestamps-20260912.md)。

- **NO_DESIGN_CHANGE / batch execution**: [批次执行表](../specs/185-prepared-model-runtime/batch-execution.md)补齐18任务12批的静态门/共享验证范围，任务卡改为实际顺序；API、owner及产品行为沿C-01–C-09，本轮不改TeX或双PDF。[证据](../specs/185-prepared-model-runtime/evidence/batch-execution-20260912.md)。

- **Core/App revision**: [C-09](../specs/185-prepared-model-runtime/contracts/core-app-boundary.md)确认协议/流/注册已有Core机制，通用运行时/等待/订阅仍需提取。新增T017/T018前置B0C，现18任务12批；模型/会话/KV语义保留DI。PLANNED，源码未迁移。[证据](../specs/185-prepared-model-runtime/evidence/core-boundary-20260912.md)。

- **Implementation design**: [C-08](../specs/185-prepared-model-runtime/contracts/code-design.md)及逐任务Design binding补齐内部类/函数/字段/流程。T016前移到B1后、T003前；具体路径/完整类型/策略来源/continuation/重载问题已修订。[证据](../specs/185-prepared-model-runtime/evidence/implementation-design-20260912.md)。产品仍PLANNED。

- **T016/B2E implementation and validation**: C-08 `CD08/CD09`, `F16`,
  `FN08`, `FLOW03`, and `PO08` are now implemented in the DI planning,
  adapter, catalog, placement, and runner boundaries. Cooperative strategy
  ports carry `ExtensionControl`; registries use explicit replace-before-freeze
  and read-only lookup; the legacy vtable remains advanced compatibility. The
  normal and clang/TSan C++ selectors plus the installed extension consumer
  pass; full packaging and later request/preparation exits remain unobserved.
  Source/evidence: T016 working-tree range from B1 base `76e656c4`,
  [B2E evidence](../specs/185-prepared-model-runtime/evidence/b2e-extensions.md),
  and [tasks](../specs/185-prepared-model-runtime/tasks.md). Status:
  `VERIFIED` for the bounded B2E exit; Spec185 remains `PLANNED` pending B2–B9.

- **Complete API revision**: [C-07](../specs/185-prepared-model-runtime/contracts/api-catalog.md)统一64组稳定入口、Python直接绑定/便利映射与值/生命周期；Subscription及局部异步等待、read取消、析构和关闭边界归原任务owner。[验证记录](../specs/185-prepared-model-runtime/evidence/api-lifecycle-20260912.md)。PLANNED，不是产品完成。

- **API revision**: [全表面审计](../specs/185-prepared-model-runtime/api-review.md)、[C-05](../specs/185-prepared-model-runtime/contracts/api-usability.md)、[C-06](../specs/185-prepared-model-runtime/contracts/cpp-first.md)。独立C++ SDK及原生异步/Provider入口，Python仅包装；任务扩为16项11批，T013完整C++资格先于T012。
- **Revision evidence**: [API修订记录](../specs/185-prepared-model-runtime/evidence/api-review-20260912.md)；源码未改，当前/冻结目标API快照不覆盖。

- **Status**: PLANNED；[Spec185](../specs/185-prepared-model-runtime/spec.md)、[任务](../specs/185-prepared-model-runtime/tasks.md)。
- **Before/after**: 已有 NativeRequestCatalog 在构造期验证冻结模型，请求期仍需model/splitter等接线；目标提供Runtime/User/PreparedModel和有界准备缓存，NativeInferenceClient继续唯一执行。
- **Contracts**: C-01公开API、C-02缓存身份、C-03请求/会话/Provider、C-04验收；详见[审计](../specs/185-prepared-model-runtime/audit.md)。
- **Design/API**: 目标roadmap新增Spec185章节；新增签名在独立中文契约，不写入已实现API inventory。当前PDF提示历史基线漂移54文件；未刷新并行源码事实。
- **Source/evidence**: 源码审计基线575b43cc93bbed29932303caf3d09974f1585af7；[规划证据](../specs/185-prepared-model-runtime/evidence/planning-20260912.md)。本轮没有native行为或资格测试，184未完成项不变。

### 2026-09-12 — Spec184 portable MiniNDN environment inputs

- **Status**: PARTIAL（environment-input unit `CLOSED_FOR_VALIDATION`）；T007 qualification remains IN_PROGRESS / PARTIAL。
- **Before/after**: MiniNDN runner 的 topology、Provider node、MiniNDN root、content store、app state 和 native executable 依赖分散在默认值与硬编码中；现在由 `ndnsf-di-minindn-environment-v1` profile 提供机器差异，显式 CLI 覆盖，启动前完成节点/binary preflight，并记录解析 profile identity。协议、ACK/Selection、Provider assembly 和 C++ API/wire 未改变。
- **Source / evidence**: source checkpoints `689cca00d40b1e2bf241aff4e7d14b0204a7c892`, `90dc8785` and `15c8abb1`; [Spec184 profile evidence](../specs/184-native-di-closure/evidence/minindn-environment-profile-20260912.md)、[environment profile guide](../docs/ndnsf-di-minindn-environment.md)。
- **PDF boundary**: no current/target API or architecture change; PDFs were not regenerated. Actual remote/Tiger and Qwen3.6-27B evidence remains external。

### 2026-09-12 — Spec184 Native MiniNDN requester route

- **Status**: PARTIAL（caller route `CLOSED_FOR_VALIDATION`）；T007 qualification remains IN_PROGRESS / PARTIAL。
- **Before/after**: MiniNDN runner previously appended `--native-cpu-provider` even when
  `--native-requester-config` was present, making the legacy per-token diagnostic branch win over
  `APPClient.request_native_reference`. The runner now selects mutually exclusive native requester
  and compatibility diagnostic arguments; User rejects the conflicting pair and normalizes the native
  final `tokenIds` response for the existing first-token oracle.
- **Current C++ boundary**: `NativeInferenceClient` still plans after `ACK_CLOSED`; the Provider keeps
  metadata-only startup slots and assembles canonical ONNX after authenticated Selection. No C++ API,
  wire contract, or Provider state-machine change was made.
- **Source / evidence**: checkpoint `7488ac08`; [Spec184 caller evidence](../specs/184-native-di-closure/evidence/native-minindn-post-ack-routing-20260912.md),
  [caller matrix](../specs/184-native-di-closure/contracts/caller-matrix.md), and [tasks](../specs/184-native-di-closure/tasks.md)。
- **PDF boundary**: no Design/API signature or target/current architecture change; current/target PDFs
  were not regenerated. The caller route and qualification limits are recorded in the active Spec evidence.

### 2026-09-11 — Spec184 Native DI Closure / Spec182 Transfer

- **Status**: NO_DESIGN_CHANGE；184 implementation NOT_STARTED；182 TRANSFERRED / qualification INCOMPLETE。
- **Before/after**: 将182剩余14个父任务、四项源码 finding 与两类闭合缺口迁入184，保留已完成实现和原始证据；
  不改变 API/wire、架构目标或当前行为，不复制历史时间线。
- **Baseline**: `94c1e644`；[184 spec](../specs/184-native-di-closure/spec.md)、
  [transfer matrix](../specs/184-native-di-closure/contracts/transfer-matrix.md)、
  [migration evidence](../specs/184-native-di-closure/evidence/migration-20260911.md)。
- **PDF boundary**: 仅文档治理/执行归属变化，双 PDF 与独立源码快照不重新生成；后续修复仍按 MANAGEMENT.md 同步。

### 2026-09-11 — Spec182 Request Chain Audit / R12 Replan

- **Status**: NO_DESIGN_CHANGE（产品实现 PARTIAL）；模块 NDNSF-DI，关联 T005/T010/T011/T013–T017。
- **Source baseline**: `72b9e388cc3920b0bdcd4c36d302d63c71e7f15a`。本轮不修改产品源码、API 或冻结设计快照。
- **Before/after**: 原执行计划累积多个旧 dispatch；现按当前请求链审计的四项缺陷和两类缺口，统一为
  R12-A–E。修复线程、既有会话提交契约和导出行为是后续 PLANNED 工作，不写为当前已修复行为。
- **Evidence**: [静态审计](../specs/182-native-di-python-bindings/evidence/request-chain-static-audit-20260911.md)、
  [R12 调度](../specs/182-native-di-python-bindings/contracts/audit-driven-execution.md)、[tasks](../specs/182-native-di-python-bindings/tasks.md)。
- **PDF boundary**: 没有 API/目标设计变更，不重新生成双 PDF，不覆盖独立冻结的当前/目标快照；
  修复实施时仍须按 MANAGEMENT.md 同步实际行为与源码摘要。本次文档 checkpoint 由 Git 历史定位。

## Spec182：Native-First Execution（2026-09-10）

- 状态 PLANNED；模块 DI；TG-02 增补独立 artifact authority 和 N1–N5 顺序。原先允许生产 requester 内置 issuer、将 process tests 统一留给 T016 的规则被取代。
- R11-B1 至 B9 与 T005/T009–T017 映射见 [执行契约](../specs/182-native-di-python-bindings/contracts/native-first-execution.md)；任务状态见 [tasks.md](../specs/182-native-di-python-bindings/tasks.md)。
- 本轮无源码/API 签名变更；当前快照和冻结目标 API 均保留。目标新增边界仍待实现，不宣称当前 requester 已移除 authority 私钥。
- 核对起点源码提交 `89932fb5b91f488eec6d4dc8756abba3fdaa40bd`；文档提交由本文件 Git 历史定位；验证见 [本轮证据](../specs/182-native-di-python-bindings/evidence/native-first-replan-20260910.md)。

## D-005：四模块图解（2026-09-08）

- 工作单元 D-DESIGN-DIAGRAMS；用户授权补充模块图、类图、时序/状态图。
- 新增图解章节与 G1--G9；API/实现行为无新增，属于现有设计的图形说明。
- 当前 G7 对照 NativeInferenceClient::dispatchOperation 的未就绪终点；目标 G7 仅表达 TG-02/TG-03，PLANNED，不声明完整原生链已实现。
- 当前源码身份与 API 清单同步，目标快照保持冻结；共享对象图核对两侧关系。
- 文档与渲染证据见 [diagram evidence](../specs/182-native-di-python-bindings/evidence/design-diagrams-20260908.md)；状态以该记录为准，不关闭 Spec182 产品任务。

## D-004：逐章修订与关键 API 行为（2026-09-08）

- 工作单元 D-DESIGN-R3；依据用户授权审计并修正 Design。
- 前：第 53 章只有 cancel 声明；grant/流/目录存在错述；BC 重复；目标正文含 R0 过时声明。
- 后：23 组 API 先解释行为，再列带所属符号的准确声明；补字段、状态表、返回/错误及调用示意。
  第 53 章覆盖 epoch/Qwen/KV/journal；BC 合入 AC，当前/目标 58/63 章；五项目标补兼容和验收。
- 新发现：Drone Execute handler 直接转交 backend，未见独立 lease/readiness 重验；
  Qwen cancel 与 handle cancel 不同；journal abort/耐久性限制按实际代码记录，目标显式承接。
- 工具：修复长方法名断行，生成两侧可读 Markdown 声明并逐字节检查，目标使用冻结 inventory。
  维护规则加入原章勘误、逐章阅读和示意/运行证据区分；不把计数当语义通过。
- 当前采样身份见 source-baseline.json 与精确补丁；目标身份继续保留独立冻结记录。
  当前新增模型/候选字段仅写当前契约，未修改目标基线或产品代码。
- 逐项位置：[67 个原主题的修订记录](reviews/chapter-revision-r3-20260908.md)。
- 验证及状态：[R3 evidence](../specs/182-native-di-python-bindings/evidence/design-r3-20260908.md)。
  文档修订不关闭 Spec182 产品任务；未定稿目标继续 PLANNED。

## D-003：逐章可理解性审阅（2026-09-08）

- 工作单元 D-DESIGN-CHAPTER-AUDIT；[审阅清单](reviews/chapter-audit-20260908.md)、
  [证据](../specs/182-native-di-python-bindings/evidence/design-chapter-audit-20260908.md)。
- 当前/目标 129 个章节位置按 67 个主题逐项阅读：KEEP 7、EXPAND 36、REWRITE 20、CORRECT 4。
- 第 53 章只展开 cancel，生成/KV/会话方法和流程缺失；第 59 章外部 grant 参数与内部 ABE
  完整策略物化混淆；目标历史说明与 TG 章节冲突。报告区分内容错误、完整性和可理解性。
- 审阅工作完成；文档内容 NEEDS_REVISION。未改产品 API、目标决策、源码、快照和 PDF。
  D-002 的技术检查为历史事实，不解释为逐章语义验收；后续修订按本清单收敛。

R2 新增 D-002（文档校验与行为补充）及 TG-01 至 TG-05（PLANNED）。目标批准来自用户
“先修复设计基线和校验机制，再补关键 API 行为契约，最后将架构改进逐项纳入目标设计”。

## D-002：基线、校验与行为契约

- 工作单元：Spec182 D-DESIGN-R2；[证据](../specs/182-native-di-python-bindings/evidence/design-r2-20260907.md)。
- 变更前：350 文件快照遗漏关键 .cpp；目标渲染共享当前 API；PDF 未绑定全部生成输入。
- 变更后：460 文件实现/配置快照；独立目标 API/源码基线；输入/PDF 构建身份验证；
  全函数 API ID 的保守覆盖状态；新增文件、源码漂移与过期生成内容均检查。
- API 行为：AC-13 修正精确 lookup；BC-01 至 BC-04 补授权失效、句柄异步行为、Repo 和 UAV 边界。
- 当前源码：ca585ab5365189203325a8462d8726d2ea32c98f 加 source-baseline.json 登记的工作区补丁；
  未提交实现只作为字节基线，不随文档暂存，不因此获得产品资格。
- 目标源码：保留 e9fe33994a6ca3ff81893591bd24c3fae43f933f 的 R1 冻结 API/源码快照。
- 文档状态和精确检查结果见证据；剩余 SIGNATURE_ONLY 项不计行为审查完成。

## R2 Target Changes

| ID | 模块 / 目标章节 | 前后变化 | API / 兼容边界 | 状态 / 后续验收 |
|---|---|---|---|---|
| TG-01 | Core / 授权版本与影响范围 | 全局版本失效 → 按权限和密钥变化区分影响 | grant/revoke/getPolicyStatus/install；旧客户端保守处理，线格式待 Spec | PLANNED；撤销、乱序、离线及无关节点刷新证据 |
| TG-02 | DI / 原生请求链 | Python/native 分担运行状态 → 原生唯一状态所有者 | APPClient/NativeInferenceClient/Handle；保留签名和错误兼容，关联 Spec182 原任务 | PLANNED；真实 requester 路径、oracle、失败及取消 |
| TG-03 | 四模块 / 异步 API | 分散描述 → 显式线程、deadline、取消、终态和背压契约 | streaming/handle/transfer/mission；逐 API 迁移 | PLANNED；竞争、重入、资源释放和远端取消 |
| TG-04 | Repo / 能力与恢复 | 模糊跨层能力 → 明确能力表和操作幂等/恢复 | lookup 精确语义不变，组合查询单独契约；格式版本迁移 | PLANNED；崩溃、重复提交、目录/数据不一致 |
| TG-05 | UAV / 类型和状态机 | Fields 与交织控制 → 类型验证、独立状态机与适配器 | command/sendMavlink/mission/job；兼容适配器明确拒绝非法输入 | PLANNED；Mock、真实飞控、失联和迟到结果分阶段 |

仅 TG-02 关联当前 Spec182 的既有迁移任务；其余尚未分配 Spec，不虚构编号。
这些记录不改变当前产品 API 或任何功能验收门；未来修改须同步对应 Spec 的 plan/contracts/tasks。

| 记录 | Spec / 工作单元 | 模块 | 设计影响 | 状态 |
|---|---|---|---|---|
| D-000 | 四模块设计 R0 建档；Spec182 的 D-DESIGN-R0 文档单元 | Core / UAV / DI / Repo | 建立当前/目标一致的 35 章基线；新增版本管理与追踪规则 | VERIFIED（文档） |
| D-182-BASE | [Spec182](../specs/182-native-di-python-bindings/spec.md) 基线观察 | DI | 记录原生迁移当前边界；尚未逐项追溯该 Spec 全部设计差异 | PARTIAL（历史映射） |
| D-001 | [D-DESIGN-API](../specs/182-native-di-python-bindings/evidence/design-api-guide-20260907.md) | Core / UAV / DI / Repo | 从组件级细化到 API 契约与准确声明，增加 AGENTS 管理要求 | VERIFIED（文档；结果见证据） |
| D-182-CC3B | [Spec182 R4-B4](../specs/182-native-di-python-bindings/evidence/r4-b4-conversation-chain-20260908.md#cc-3b-requester-provider-transaction-wiring) | DI | 配置化 native requester 接入会话 owner、认证 receipt、Provider COMMIT/ROLLBACK/FINALIZE 与终态清理；真实跨进程两轮仍待验收 | PARTIAL（实现与局部验证） |

## D-001：API 开发者指南与维护规则

- 依据：用户要求 API 级细节并参考 NFD Developer’s Guide；本次不实现新产品 API。
- 变更前：35 章组件设计，API 主要作为源码定位入口。
- 变更后：增加第 36–58 章 API 契约、295 文件声明参考、完整签名/默认值/类型字段与契约映射；MANAGEMENT.md 和本机 AGENTS.md 规定每个 Spec/API 变化的同步、验证与提交流程。
- 当前/目标：本轮两份一致；目标契约独立维护，允许后续加入明确标识的计划接口。
- 实现状态：文档实现；原生 DI requester 等产品未完成项保持原状态。5738 个函数条目不代表 5738 个接口均已运行验证。
- 源码身份：R1 source-baseline.json 与精确补丁，可从 Git 还原 350 个文件；API inventory 逐文件绑定其中 295 个文件。API ID 列表见 contract-map.json。
- 验证与下一步：[API 指南证据](../specs/182-native-di-python-bindings/evidence/design-api-guide-20260907.md)。后续每个 Spec 按 MANAGEMENT.md 同步，不自动覆盖目标。

## D-000：四模块设计建档

- 日期：2026-09-07。
- 依据：用户要求建立当前/目标设计，并追加要求按 Spec 追踪、完整纳入 Git。
- 章节：两份文档第 1–35 章；本次维护修订涉及“文档范围与源码基线”和“文档维护与后续目标变更”。
- 变更前：没有统一的四模块双份设计基线；首版生成后暂时本地排除。
- 变更后：统一保存两份中文 PDF、独立可编辑正文、模块清单、源码摘要和本文件，全部纳入 Git；初始目标技术正文等于当前正文。
- 运行行为变化：无；本单元不修改产品代码。
- 源码基线：`d7fa9c8edef924a6cf29c12d3793049aa794a6be` 加 [快照补丁](evidence/source-baseline-worktree.patch)，对应 [94 文件摘要](source-baseline.json)。不把工作树快照误写成已提交实现。
- 上述 R0 的摘要/补丁指提交 9c019a17 中的同名文件；当前路径已在 R1 推进，回溯时须从该历史提交读取。
- 验证：[文档验证](validation.md)、[Spec 文档工作单元](../specs/182-native-di-python-bindings/evidence/design-pdf-baseline-20260907.md)。文档状态 VERIFIED 不代表四模块运行资格已全部通过。

## D-182-BASE：原生 DI 迁移的当前设计边界

- Spec：[182-native-di-python-bindings](../specs/182-native-di-python-bindings/spec.md)；任务依据：[tasks.md](../specs/182-native-di-python-bindings/tasks.md)。
- 章节：第 18–23 章 DI，第 31 章“实现状态与验证口径”。
- 已核对现状：Python 应用规划与 C++ Provider 执行组件并存；原生 NativeInferenceClient dispatch 返回 NATIVE_REQUEST_PIPELINE_NOT_READY，准备/放置/封存组件的存在不等于完整请求链已接通。
- 当前/目标差异：本次用户指定 R0 两份一致，因此没有把 Spec182 尚未完成的迁移目标自动写入目标 PDF；Spec182 的目标仍以其 plan/contracts 为准。
- 历史变更前后与提交范围：尚未完整追溯，不能把 R0 快照当作整个 Spec182 的变更清单。下一次该 Spec 设计更新时新增具体条目，并逐项补齐任务 ID、行为差异、源码提交与验证。
- 状态：PARTIAL 仅指本文件的历史映射；不替代或降低 Spec 自身验收状态。

## D-182-CC3B：会话请求事务接线

- 日期 / Spec / 任务：2026-09-08；Spec182 R4-B4 CC-3B；T011-C 保持 PARTIAL。
- 原设计：`NativeInferenceClient` 仅保存会话 coordinator，公开 stream final 没有 receipt
  收集、Provider promotion 或 durable checkpoint 提交。
- 当前变化：配置了 `NativeRequestRuntime` 的请求在 final 阶段创建 owner turn，验证每个角色的
  receipt，发送加密 COMMIT/ROLLBACK/FINALIZE 控制并等待 canonical commit ACK；coordinator
  在 durable gate 中执行 parent/journal 晋升，取消、deadline 和 replacement 清理有明确边界。
  未配置 runtime 的兼容/组件构造仍保留结构化 `NATIVE_REQUEST_PIPELINE_NOT_READY`。
- 兼容与目标边界：不改变既有 collaboration wire；Provider 仍使用现有 request-scope 加密
  端口。真实两轮、跨进程 control/receipt、恢复和 T016 qualification 未完成，因此不能把局部
  build/test 结果写成完整原生请求链。
- 源码与证据：NativeInferenceClient、NativeConversationCoordinator、NativeProviderHandler；
  当前批次命令、日志和剩余出口见 [R4-B4 证据](../specs/182-native-di-python-bindings/evidence/r4-b4-conversation-chain-20260908.md#cc-3b-requester-provider-transaction-wiring)。
- Design PDF：当前/目标 PDF 继续保持原设计基线；未将未验收的请求链写入目标行为，下一次设计
  PDF 修订须在真实两轮/恢复验收后同步。
- 状态：PARTIAL；下一步补 C++ integration harness，再执行 T012/T013 与 T015/T016。

## D-184：Spec184 原生 YOLO 请求与候选绑定收敛

- 日期 / Spec / 任务：2026-09-11；[Spec184](../specs/184-native-di-closure/spec.md)；T007-A0/A1/A2。
- 模块 / 当前章节：NDNSF-DI User/Provider request-scope、native ONNX assembler、qualification harness。
- 原设计与变化：请求作用域输入原先可由多个 Provider 共用同一逻辑路径；当前实现为每个 Provider
  建立独立的 key/binding/input name 状态，并在清理时逐项失效。`COMPONENT_SET` assembler
  原先按完整图节点数拒绝子集；当前按声明的 role boundary 与 extracted graph 验证 certified
  subset，同时保留 deterministic node bytes 与 node-index cover 检查。
- 当前实现 / 目标边界：Y-A 单 Provider、Y-B 多 Provider、Y-N 七个拒绝边界已由当前候选的
  C++ 生产路径运行验证；Python 只编排 MiniNDN 和收集证据。Qwen3.6-27B、继承 negative/
  retirement、I05、Python retirement 与 SIF/Tiger 仍未实现或未资格化，不能写入当前行为。
- 兼容性 / 安全：保留现有 collaboration wire；request-scoped input name 增加 Provider
  绑定，防止跨 Provider key/state 复用；ONNX subset 只允许 recipe 声明和图输入/role boundary
  可达的节点，非法 cover 继续拒绝。
- 源码范围：`ServiceUser.hpp/.cpp`、`ServiceProvider.hpp/.cpp`、
  `NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.cpp`；验证 harness
  为 `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`。
- 验证与证据：[T007 process qualification](../specs/184-native-di-closure/evidence/t007-process-qualification-20260911.md)、
  [remainder audit](../specs/184-native-di-closure/evidence/remainder-audit-20260911.md)、
  `.codex-tmp/spec184-yolo-Y-A-output-20260911-r51/`、
  `.codex-tmp/spec184-yolo-Y-B-output-20260911-r37/`、
  `.codex-tmp/spec184-yolo-Y-N-output-20260911-r50/`；candidate receipt verify exit `0`，
  Y-A/Y-B/Y-N `PASS`，`git diff --check` 和 Python syntax check 通过。
- 状态：`PARTIAL`；本地 C++/YOLO 行关闭为 `PASS_FOR_ROW`，Qwen3.6-27B 明确
  `WAITING_EXTERNAL_INPUT`。下一步仅处理 A4 继承行与外部模型/实验机，不把 0.6B smoke 代替 27B。

## 新记录模板

### D-185-B7R：runner identity 与 exact-forward cache 生命周期契约

- 日期 / Spec / 任务与契约 ID：2026-09-15；[Spec185](../specs/185-prepared-model-runtime/spec.md)；T021 / C-03、C-08。
- 模块 / 当前与目标章节：NDNSF-DI `NativeModelRunner` 与 `ProviderRoleWorker`；当前 API reference 的 `NativeModelRunner` 条目。
- 原设计 / 新设计 / 修改原因：exact-forward cache 原先用 runner 原始地址区分实例；生产两次独立授权请求中 allocator 复用地址会错误命中已销毁 runner 的输出。当前改为进程内外部 registry 分配单调 runner identity，并在 base destructor 移除登记；cache key 使用该 identity。公共多态基类不增加数据成员，保持对象布局不变。
- 当前已实现部分 / 目标未实现部分：normal 与 ASan/UBSan C++ protected Provider selector 观察两次独立 grant 各自 source fetch、assembly、runner creation 和 execution；生产 grant issuance 与真实 ONNX Runtime 模型仍由 fixture/后续资格批次覆盖。
- 兼容性、迁移或撤回影响：新增构造、析构和 identity accessor 已登记到当前 DI API inventory/reference；无 wire 字段变化，旧 runner 地址不再作为 cache identity。
- 源码提交或范围 / 文档提交定位：`NativeModelRunner.hpp/.cpp`、`ProviderRoleWorker.cpp`、T021 C++ selector；本地 checkpoint 待组合门通过后提交。
- 验证命令、结果与持久证据：`.codex-tmp/spec185-t021-runtime/production-independent-grants-normal-v5.log` 与 `production-independent-grants-asan-v3.log` 均 `RC=0`；详见 [B7R T021 evidence](../specs/185-prepared-model-runtime/evidence/b7r-lifecycle-fixes-20260915.md#t021-follow-up-production-protected-independent-grant-matrix)。
- 状态：PASS（T021）；T013、T012、T014 仍按各自验收边界保持未完成。

### D-185-B4：会话追加输入的 native adapter token 契约

- 日期 / Spec / 任务：2026-09-13；[Spec185](../specs/185-prepared-model-runtime/spec.md)；T007/T008 B4。
- 设计变化：取消调用者可写的 `RequestOptions.canonicalTokenIds`，改由已验证 native adapter
  的 `conversationInputTokens(Input)` 生成当前输入 suffix；native `Conversation` 在首轮建立
  token prefix，在 `APPEND_DELTA` 接到 durable parent 后再由 coordinator 校验严格增长。
- 原因与边界：此前公开包装没有把本轮输入的 canonical token 谱系传到 coordinator，第二轮只能在
  `beginTurn` 被正确拒绝；直接增加公开 vector 会允许调用者伪造 lineage，因此改为 adapter-owned
  encoder。该机制不是新的 parent receipt、role map、plan 或 Provider 参数；没有 pinned encoder
  的 adapter 必须显式报 `UNSUPPORTED_CAPABILITY`。
- 源码与契约：`NativePlanning.hpp`、`NativeCatalogModelAdapter.*`、
  `NativeCanonicalPreparationCatalog.*`、`NativeRequestCatalog.cpp`、`PreparedModel.*`、
  `Conversation.hpp/.cpp`、B4 C++ fixture；对应
  `contracts/public-api.md`、`execution.md`、`api-catalog.md`、`code-design.md`、`api-usability.md`。
- 验证边界：旧 B4 r5 失败证据保留于 [b4-conversation](../specs/185-prepared-model-runtime/evidence/b4-conversation.md)；
  新字段仅完成静态复审准备，normal/sanitizer selector 尚未重跑，B4 仍 `PARTIAL`。当前/目标 PDF
  在 B4 通过并进入文档交付时按 MANAGEMENT.md 统一刷新，未把本次未验收行为写成资格 PASS。

### D-185-B8/B9：PreparedModel 原生入口与设计交付快照

- 日期 / Spec / 任务与契约 ID：2026-09-15；[Spec185](../specs/185-prepared-model-runtime/spec.md)；T012/T014 / C-05、C-06、C-07、C-08。
- 模块 / 当前与目标章节：NDNSF-DI `Runtime`、`User`、`PreparedModel`、`NativeInferenceClient`、pybind facade；Design 当前/目标 API、G7 图与双 PDF。
- 设计变化：当前设计从“原生 dispatch 尚未接通”的历史快照更新为区分两条实际路径：`Runtime::open → User::prepare → PreparedModel → NativeInferenceClient` 已由 C++ 过程矩阵和 Python 薄绑定验收；没有 preparation/request contract 的兼容构造仍返回 `NATIVE_REQUEST_PIPELINE_NOT_READY`。目标设计继续独立保留 TG-01--TG-05 的 `PLANNED` 内容。
- 修改原因：T013/T012 已形成当前候选的原生过程与包装边界，旧当前书会把已验证实现误报为未实现；API 清单、绑定映射、行为覆盖和源码快照也需要绑定同一工作树身份。
- 当前已实现 / 目标未实现：B7 C++ 过程矩阵、B8 Python 绑定和 lifecycle 修复有各自证据；subinterpreter、wheel packaging、Spec184 外部模型/retirement、I05 与 SIF/Tiger 仍保持未观测或 `PARTIAL`。
- 兼容性 / 撤回：保留旧 `APPClient` 编排和无 runtime 的兼容失败语义；未新增 wire 字段。若回退文档，只能回退当前设计说明，不得覆盖目标快照或降低已记录的 C++ 资格证据等级。
- 源码范围 / 文档提交定位：`Design/api/*`、`Design/current-api.tex`、`Design/current-content.tex`、`Design/current-design.tex`、`Design/current-diagrams.tex`、`Design/diagrams/di-current-flow.tex`、`Design/api-contracts.json`、`Design/validation.md` 与 `specs/185-prepared-model-runtime/evidence/b9-handoff.md`；源码身份由 `Design/source-baseline.json` 和 `evidence/source-baseline-worktree.patch` 绑定。
- 验证命令、结果与持久证据：`test_design_state.py`、`verify-api-reference.py`、`verify-source-baseline.py` 均 `PASS`；双 PDF 构建和 `verify.py` 均 `PASS`，构建目录 `.codex-tmp/design-pdf-20260915T130303244487Z/`，详见 [B9 handoff](../specs/185-prepared-model-runtime/evidence/b9-handoff.md)。
- 状态：`PASS`（文档交付）；这不升级 Spec184 的外部资格，也不把文档门替代 native C++ 运行验收。

### D-编号：设计变化名称

- 日期 / Spec 链接 / 任务与契约 ID：
- 模块 / 当前与目标章节标题：
- 原设计 / 新设计 / 修改原因：
- 当前已实现部分 / 目标未实现部分：
- 兼容性、迁移或撤回影响：
- 源码提交或范围 / 文档提交定位：
- 验证命令、结果与持久证据：
- 状态 / 剩余验收 / 下一步：

### D-187：YOLO MiniNDN 原生 User selector 接线

- 日期 / Spec / 任务与契约 ID：2026-09-15；[Spec187](../specs/187-yolo-minindn-sif-app/spec.md)；T002/T006 / FR-005、FR-006、FR-012。
- 模块 / 当前与目标章节：`Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`、`tests/integration-tests/di-prepared-request.t.cpp`、`tests/wscript`；YOLO MiniNDN local acceptance。
- 原设计 / 新设计 / 修改原因：原接线在 Python MiniNDN case 完成后另起 `DummyClientFace` C++ fixture，且 selector 可缺省，不能证明同一网络调用链。当前在显式 `SPEC187_NATIVE_MODE=1` 下由 runner 在网络启动前校验 selector/config/input/output，并把注册的 C++ `NativeRequesterThroughMiniNdn` 作为 MiniNDN User 子进程启动；C++ 直接调用 `Runtime::open → User::prepare → PreparedModel::request`，通过继承的 `NDN_CLIENT_TRANSPORT` 连接节点 NFD。
- 当前已实现部分 / 目标未实现部分：C++ target、source closure、fail-closed 输入门和独立 served-provider selector 已通过；Requester 的 ACK/Selection 与 Provider 的 Selection/execution 阶段记录按 request/attempt/plan 关联并由 C++ selector 校验顺序；真实 candidate-bound config、两次 local MiniNDN run、SIF/Tiger promotion 尚未观测。
- 兼容性、迁移或撤回影响：未改变 NDNSF wire 或默认旧 Python harness；只有显式 Spec187 native mode 使用 C++ User，缺失 selector/config/input/output 不回退到 Python PASS。
- 源码提交或范围 / 文档提交定位：本地 T002 工作区改动；[B187-LOCAL-YOLO evidence](../specs/187-yolo-minindn-sif-app/evidence/b187-local-yolo.md)、[convergence report](../specs/187-yolo-minindn-sif-app/evidence/convergence-20260915-r1.md)。
- 验证命令、结果与持久证据：`spec187-yolo-minindn` normal compile/link `rc=0`（`-j4`，1m6.118s）；独立 served-provider C++ selector `rc=0`；缺失 native input selector 以 rc=201 fail-closed；官方 review-agent r7 `STATIC_PASS`，阶段证据详见 B187-LOCAL-YOLO。
- 状态 / 剩余验收 / 下一步：`PARTIAL`；等待 regular base SIF、host-gate manifest 和 native requester config/input 后执行两次 local gate，再决定 TigerCluster promotion。

历史 Spec 的完整回溯是后续独立核对工作，本轮不虚构它们的变更记录。

### D-187-BASE: Stable dependency image repair

- 2026-09-15；Spec187 T001 prerequisite；`NO_DESIGN_CHANGE`：仅修复部署脚本及稳定依赖镜像，不修改 Core/UAV/DI/Repo API、wire 或目标设计。
- 保持现有稳定 base / 外置 APP 边界；NumPy wheel 私有库完整性与最终镜像 C++ SDK smoke 见 [base repair](../specs/187-yolo-minindn-sif-app/evidence/b187-base-repair.md)。本机最终镜像 `BASE_SMOKE_ONLY PASS`，缺库反例被拒绝；不以此刷新生产 API 或将 APP/MiniNDN 状态升级为 PASS。

## D-187-SEGMENT：request-scoped large input 的标准 segmented Data 绑定

- 日期 / Spec / 任务与契约：2026-09-16；[Spec187](../specs/187-yolo-minindn-sif-app/spec.md)；T002；request-scoped input transport。
- 原设计与变化：6.55 MB 输入曾走单个 request-scoped Data，超过 8,800-byte transport bound。当前大输入按 4,096-byte encrypted chunks 发布为 `base/version/segment` Data，User 与 Provider 用 `.appendVersion(attempt)` 共享基础名；每段独立 AEAD AAD，统一 `FinalBlockId`，Provider 以 `SegmentFetcher` 组装后才解密/派发。小输入保留单 Data 路径。
- 原因与兼容：Segmenter/SegmentFetcher 的 NDN object contract 需要 version component；attempt 派生版本不增加 wire 字段，也不改变 request/Selection 语义。旧单 Data 失败记录保留，不能把旧路径作为当前行为。
- 源码与证据：`ndn-service-framework/ServiceUser.cpp`、`ServiceProvider.cpp`、`tests/integration-tests/request-scoped-selection.t.cpp`；[segmented regression](../specs/187-yolo-minindn-sif-app/evidence/b187-local-yolo-recheck-20260916.md#2026-09-16-c-segmented-request-regression)。
- 验证：官方 `review-agent` snapshot `review-segmented-input-20260916-r3` 返回 `STATIC_PASS`；`build-spec187-local-nac-r1` integration target `-j4` `rc=0`；`RequestScopedSelection/*` 与 `RequestScopedResponseConfidentiality/*` 第二次整套均 `rc=0`。长输入断言 1,601 segments、统一 FinalBlock、最大 wire <8,800 bytes 和完整组装后 handler。缺段/乱序/错误 FinalBlock/超时负例仍未运行。
- 当前/目标边界：C++ DummyFace 分段边界已通过；当前源码 MiniNDN r42 Controller native crash、SIF/APP 与 Tiger qualification 仍独立 `PARTIAL`，不因本条升级。

## D-189：Qwen two-provider MiniNDN full-path gate

- 日期 / Spec / 任务与契约：2026-09-18；[Spec189](../specs/189-qwen-two-provider-minindn/spec.md)；T001–T010；`Q189-PREP`、`Q189-REPO`、`Q189-WIRE`、`Q189-ASSEMBLY`、`Q189-HANDOFF`。
- 原设计与变化：Spec188 的 bounded core 以 YOLO 两轮作为本地出口，Qwen 只保留为 follow-up resource probe，不能证明最初的多 Provider CPU 目标。Spec189 独立规定真实 Qwen3-0.6B 的 `prepare → Repo manifest/layer publication → reference-only request → ACK → Selection → placement-bound Provider fetch/assembly/execute → terminal → drain` 全链；prepare 一次发布、后续 request 复用 reference，不能使用 stage export、单 Provider ORT、预置 runner 或 synthetic ACK/Selection 替代。
- 当前已实现部分 / 目标未实现部分：本机已生成并校验两阶段 ONNX、external canonical graph 和 initializer，哈希及大小见 [B189-1 evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-prepare.md)。native prepare/Repo commit、真实 ACK/Selection、两个 Provider 的 C++ handoff/execute、MiniNDN 和 repeat 尚未运行，Spec189 保持 `IN_PROGRESS`。
- 兼容性、迁移或撤回影响：不修改 Spec188 已关闭的 bounded core 结论；`.specify/feature.json` 和 `AGENTS.md` 的 active pointer/rules 指向 Spec189。任意模型、split、ABI、profile、selector 或 handoff 变化都使后续证据失效。
- 源码范围 / 文档定位：`specs/189-qwen-two-provider-minindn/{spec,plan,tasks,batch-execution,contracts,quickstart}.md`；通用实验规则写入 `AGENTS.md` 的 `Real multi-provider model experiment contract`。
- 验证：`verify-spec-kit-sync.py --require-entrypoints` 返回 `PASS: 11/11`；ONNX checker 和 ORT CPU session 通过；这些仅是制品准备证据，不是 native/MiniNDN PASS。原始导出日志保留在 `.codex-tmp/spec189-qwen-two-provider-20260918/`。
- 状态 / 下一步：`PARTIAL` / `IN_PROGRESS`；先执行 B189-0 的 CodeGraph/ABI/handoff 冻结，再按 T002/T003 让 native prepare 把已验证制品发布到 Repo，随后才可运行双 Provider MiniNDN。

### D-189-PROGRESS：跨 Provider progress 与 Qwen/YOLO 验收边界

- 日期 / Spec / 任务与契约 ID：2026-09-20 04:34 -0500；[Spec189](../specs/189-qwen-two-provider-minindn/spec.md)；T006/T007/T009；FR-030、SC-007。
- 模块 / 当前与目标章节：Core `ServiceUser`/`InvocationStream` collaboration stream consumer、NDNSF-DI Provider assembly progress；Spec189 当前资格边界。
- 原设计 / 新设计 / 修改原因：原运行时只按 terminal Provider 的 progress 续命，Qwen 的非 terminal Provider 在 selected-material assembly 或 hidden-state 等待期间会触发 stream gap。现行契约按 committed Selection 接受每个 `{providerName, providerSelectionDigest, role operationId}`，并分别检查 `(epoch, sequence)` 新鲜度；YOLO 的轻量 `NATIVE_POSTPROCESS` 结果不再作为 Qwen 两 Provider 资格证据。
- 当前已实现部分 / 目标未实现部分：r47/r70 已证明 Qwen 进入 ACK、Selection、grant verification 和 selected-material fetch；progress binding 的 C++ 状态机与 focused selectors 已通过。新安装候选的真实 MiniNDN runner、hidden-state handoff、terminal output、cleanup 和重复请求仍未完成。
- 兼容性、迁移或撤回影响：单 Provider 继续使用一个精确 operation binding；不改变 ACK/Selection wire 或授权语义。宿主机 `swapIo`/`diskFree` 仍作为独立 `RESOURCE_BOUNDARY`，不能与协议失败合并。
- 源码范围 / 文档提交定位：`ndn-service-framework/ServiceUser.cpp`、`ndn-service-framework/InvocationStream.cpp/.hpp`、`specs/189-qwen-two-provider-minindn/{spec,plan,tasks,batch-execution,traceability}.md`；证据为 [r4 progress heartbeat](../specs/189-qwen-two-provider-minindn/evidence/b189-r4-progress-heartbeat-20260919.md) 与 [r70 boundary](../specs/189-qwen-two-provider-minindn/evidence/b189-native-r70-progressed-stream-boundary-20260919.md)。
- 验证命令、结果与持久证据：C++ progress/lifecycle selectors 已通过并获只读 `STATIC_PASS`；r70 仍使用旧候选并保持 `PARTIAL`，必须用新安装候选重跑，不能由静态或 YOLO 结果升级。
- 状态 / 剩余验收 / 下一步：`PARTIAL`；先完成 affected Core/DI 安装身份核对，再进行一次新的真实 MiniNDN run；只有越过 stream gap 后才继续材料、runner、handoff 或 terminal 边界。

### D-189-CACHE：Selection 后 stable-root immutable assembled artifact reuse

- 日期 / Spec / 任务与契约 ID：2026-09-20；[Spec189](../specs/189-qwen-two-provider-minindn/spec.md)；T003/T006/T007；FR-010、FR-015、`Q189-ASSEMBLY`。
- 模块 / 当前与目标章节：`NativeCanonicalOnnxAssembler`、`Provider`、Spec189 placement/cache contract；Selection 后 Provider materialization 与 bounded cache。
- 原设计 / 新设计 / 修改原因：原实现只有 Provider 进程内 `ProviderArtifactCache`，实验入口又把 artifact root 放在每个 run 下，导致重启后即使已有完整 assembled model 也会重新 fetch/copy/assemble。当前增加 stable system root 扫描：先验证当前 authenticated Selection/grant，再按 role 读取 manifest，逐块校验实际 `model.onnx` SHA-256 与完整 immutable identity，命中后直接复用现有路径。篡改/缺失/半写入 entry 回到 cold path；protected grant-bound ciphertext 不跨请求复用。
- 当前已实现部分 / 目标未实现部分：C++ loader、Provider production wiring、stable Qwen launcher root 与 focused digest/tamper regression 已实现并通过 focused selector；完整 MiniNDN warm hit、两 Provider execute、terminal、repeat、资源峰值与 qualification 仍未观测。
- 兼容性、迁移或撤回影响：不新增 llama adapter，不改变 ONNX input/KV/logits contract、ACK/Selection/grant wire 或 KV state；旧 run-scoped cache 不自动迁移，缺少新 manifest identity 的 entry 只会 cold miss。回退时删除 loader/wiring 并恢复 run-scoped launcher path，不影响现有 authenticated cold path。
- 源码范围 / 文档提交定位：`NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.{hpp,cpp}`、`Provider.cpp`、`examples/DI_NativeProviderExecutable.cpp`、`Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py`、`tests/integration-tests/di-prepared-provider.t.cpp`、`di-prepared-process.t.cpp`；对应 Spec189 plan/tasks/placement contract。
- 验证命令、结果与持久证据：`./waf build --targets=spec185-provider-assembly -j4` `rc=0`；`NDNSF_SPEC182_BIN_DIR=build-spec189-oracle build-spec189-oracle/spec185-provider-assembly --run_test='Spec185ProviderAssembly/Spec188ProviderReferenceAssembly/ProductionAssemblerCache*' --log_level=test_suite` 两 case `rc=0`；首次未设置 worker 环境的 invocation boundary 见 [cache selector evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-cache-selector-worker-path-20260920.md)。
- 状态 / 剩余验收 / 下一步：`PARTIAL`；先核对安装候选并以稳定 root 运行一次 warm/cold 对照，确认 Provider 记录 cache hit 且不出现重复 material fetch/assembly，再继续真实 MiniNDN full-path gate。

### D-189-STREAM：Selection 后才启动 streamed event prefetch

- 日期 / Spec / 任务与契约 ID：2026-09-20；[Spec189](../specs/189-qwen-two-provider-minindn/spec.md)；T006/T007；`Q189-WIRE`、`Q189-ASSEMBLY`。
- 模块 / 当前与目标章节：Core `ServiceUser::initializeStreamConsumer` 与 Selection publication；Spec189 request-chain stream boundary。
- 原设计 / 新设计 / 修改原因：原实现创建 streamed event consumer 后立即调用 `prefetchWindow()`，在 Provider 尚未收到 authenticated Selection 时就发出 exact event Interests，可能把正常的 admission wait 误判为 `stream event gap exceeded retry budget`。当前由调用方在 Selection 成功上线路径后再启动 ordinary/collaboration consumer；已有 binding 的 targeted request 在初始化返回后立即启动，保持其既有 Selection 顺序。
- 当前已实现部分 / 目标未实现部分：三条 Core Selection 路径已加入 Selection-gated prefetch，targeted/normal/collaboration focused selectors 和既有 native flow selector 通过；新的 Qwen r152 已越过 ACK/grant verification，但仍未观察到 Selection，故完整 Qwen handoff、cache hit、execute、terminal 和 repeat 仍未实现。
- 兼容性、迁移或撤回影响：不改变 Selection/ACK wire、授权、KV 或 ONNX contract；失败时仍保留 bounded stream retry/cancellation 语义。回退仅恢复 consumer 初始化时机，不影响稳定 cache 的 digest/authorization gate。
- 源码范围 / 文档提交定位：`ndn-service-framework/ServiceUser.cpp`；验证记录与当前失败边界见 [r152 evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-r152-qwen-selection-boundary-20260920.md)。
- 验证命令、结果与持久证据：`./waf --out=build-spec189-oracle build --targets=integration-tests -j4` `rc=0`；`Spec175InvocationStream/NormalServiceOnlyRequestPublishesOrderedEventsAndOneResult`、`Spec175InvocationStream/TargetedStreamBootstrapsAndUsesOneSelection`、`Spec170NdnsfDiCoreFlow/ProductionNativeHandlersRunStreamedD2bRequestToFinalResponse`、`Spec170NdnsfDiCoreFlow/PreconfiguredEnvironmentRunsFourProviderRoleSplitCollaboration` 均 `PASS`。真实 Qwen r152 仍为 `PARTIAL`，未宣称资格 PASS。
- 状态 / 剩余验收 / 下一步：`PARTIAL`；先定位 Selection publication/consumer 的实际 wire boundary，再运行 stable-root warm/cold 对照和完整 two-provider handoff。

### D-189-SOURCE-CACHE：system-wide canonical source Repo reuse

- 日期 / Spec / 任务与契约 ID：2026-09-20；[Spec189](../specs/189-qwen-two-provider-minindn/spec.md)；T003/T005/T006；`Q189-PREP`、`Q189-REPO`、`Q189-ASSEMBLY`。
- 模块 / 当前与目标章节：`Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py`、requester canonical source Repo；prepare-time source ownership and cache boundary。
- 原设计 / 新设计 / 修改原因：原 launcher 将 canonical source Repo 放在每个 run 的 `requester/canonical-repo`，导致相同 digest 的 graph/initializer 在重启时重新 ingest 到新的磁盘目录。当前按 model manifest、source/graph/initializer、node-mapping 和 tokenizer digest 派生系统唯一 namespace，在运行前校验 content-addressed payload 的大小与 SHA-256；命中时复用 Repo，缺失对象才走现有 fallback ingest。
- 当前已实现部分 / 目标未实现部分：source cache identity、原子 marker、hash/size 校验和 requester 配置接线已实现；r155 已完成首次 source Repo 写入并证明 ACK/Selection/grant 可继续运行。Provider 侧 encrypted transport 仍保持 run-scoped，protected grant-bound material 不跨请求复用；assembled role cache、runner、terminal 和 warm-hit 仍未完成验收。
- 兼容性、迁移或撤回影响：不改变 NDNSF wire、ACK/Selection/grant、ONNX input/KV/output 或 llama adapter；不同候选进入不同 digest namespace，identity mismatch fail-closed。旧 run-scoped source Repo 不自动迁移，也不删除历史 raw evidence。
- 源码范围 / 文档提交定位：`Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py`、`tests/python/test_spec189_model_source_cache.py`；[r155 source-cache evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-r155-source-cache-memory-boundary-20260920.md)。
- 验证命令、结果与持久证据：`py_compile` 通过；`pytest -q tests/python/test_spec189_model_source_cache.py` 为 `2 passed`；r155 preflight emitted `MODEL_SOURCE_CACHE` and persisted both expected source objects. The same run stopped at `RESOURCE_BOUNDARY:MemAvailable` during Provider material/assembly, so this focused source-cache result is not a product qualification PASS。
- 状态 / 剩余验收 / 下一步：`PARTIAL`；先完成 assembly memory-peak/cache-owner diagnosis，再做一次 source-cache `hashesVerified=true` preflight and only then attempt a warm Provider path if the assembled cache is complete.

### D-189-ASSEMBLY-RELEASE：释放 source protobuf 后进入 assembled runtime

- 日期 / Spec / 任务与契约 ID：2026-09-20；[Spec189](../specs/189-qwen-two-provider-minindn/spec.md)；T003/T006/T007；`Q189-ASSEMBLY`。
- 模块 / 当前与目标章节：`NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.cpp`；Provider assembly working-set boundary。
- 原设计 / 新设计 / 修改原因：原 assembler 在 extraction 后继续持有完整 authenticated source `ModelProto`，同时保留选定 role 的 assembled graph，并经过二次 checker、wire serialization 和 ORT session load，造成 source、selected model 和 runtime buffers 的重叠 working set。当前在 extraction 完成且所需字段已复制后，显式 swap/release 完整 source protobuf，再进入 S6/S7/runtime phase；不改变 assembled graph、digest、ONNX input/KV/output 或 wire contract。
- 当前已实现部分 / 目标未实现部分：source release 已通过 `Spec182OnnxActivation/ActivationAssemblesRealModelThroughWorkerFixedManifest` 和 `Spec182OnnxExtraction` `11/11` focused checks；r156 真实 run 越过 r155 的 `MemAvailable` boundary 并到达 Provider-0 `RUNNER_READY`。完整 two-provider terminal、model output、warm assembled cache、repeat 和 qualification 仍未完成。
- 兼容性、迁移或撤回影响：只改变 protobuf lifetime/peak working set，不改变 cache identity、Selection/grant、role assignment、lineage 或 ONNX contract；失败时仍由既有 assembly/runner validation fail closed。
- 源码范围 / 文档提交定位：`NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.cpp`；[r156 evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-r156-worker-release-lineage-boundary-20260920.md)。
- 验证命令、结果与持久证据：affected DI native closure built/installed with `-j4`; activation selector `PASS`; extraction selector `11/11 PASS`; r156 resource guard `boundary=null`, `cleanup=PASS`, `min MemAvailable=2326757376`, `maxOwnedSwapBytes=205512704`。其后的 first product boundary 是 `GenerationEpochLineageV1 invalid producerRole`，所以不能把本轮记为 full qualification。
- 状态 / 剩余验收 / 下一步：`PARTIAL`；先修复 generation lineage edge binding，再用新 run ID 复测同一 source-cache/assembly path。

### D-189-LINEAGE-CORE：区分 core lineage 与 edge-local routing 校验

- 日期 / Spec / 任务与契约 ID：2026-09-20；[Spec189](../specs/189-qwen-two-provider-minindn/spec.md)；T006/T007；`Q189-ASSEMBLY`、`Q189-WIRE`。
- 模块 / 当前与目标章节：`GenerationEpochLineageV1`、`OnnxRuntimeModelRunner`；Provider 本地 ONNX position materialization 与 encrypted dependency edge publication。
- 原设计 / 新设计 / 修改原因：原 `materializeCausalPositionInputsV1()` 在 Provider 尚未绑定 edge-local `producerRole`/`consumerRole` 时调用完整 `validate()`，误拒绝带有合法 request/generation core state 的初始 lineage。当前增加 `validateCore()`，仅本地 position derivation 使用 core 校验；wire encode/decode 和 edge publication 继续要求完整 producer/consumer routing 校验。
- 当前已实现部分 / 目标未实现部分：校验范围修正和 r161 首个失败边界已确认；新的 affected DI 构建与真实 r162 two-provider run 尚未完成，因此 terminal、model output、repeat、resource envelope 和 qualification 仍未完成。
- 兼容性、迁移或撤回影响：不改变 lineage wire 字段、digest、Selection/grant 或 ONNX input/KV/output contract；完整 wire validation 保持 fail-closed。撤回时恢复本地 materializer 的 full validation，但会重新触发 r161 的初始 edge-less lineage rejection。
- 源码范围 / 文档提交定位：`NDNSF-DistributedInference/cpp/ndnsf-di/GenerationEpochLineage.hpp`、`TensorBundleCodec.cpp`、`NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.cpp`；证据为 [r161 boundary](../specs/189-qwen-two-provider-minindn/evidence/b189-r161-lineage-validation-diagnostic-20260920.md)。
- 验证命令、结果与持久证据：r161 已通过合法日志、source-cache hit、真实 MiniNDN 到 Provider assembly/runner 边界；`ndnsf-distributed-inference` 与 `di-native-provider` 已用 system compiler/binutils、`-j4` 成功构建并安装；r162 runtime 验证待执行。
- 状态 / 剩余验收 / 下一步：`PARTIAL`；重建/安装 affected DI targets，以新 run ID 验证 local materialization 后的 edge binding、Provider-1 assembly、terminal response 和 cleanup。

### D-189-CACHE-COMPAT：Selection 后临时跳过 Repo material fetch 的缓存兼容诊断

- 日期 / Spec / 任务与契约 ID：2026-09-20；[Spec189](../specs/189-qwen-two-provider-minindn/spec.md)；T006/T009；FR-031、`Q189-REPO`、`Q189-ASSEMBLY`。
- 模块 / 当前与目标章节：`NativeCanonicalOnnxAssembler`、`DI_NativeRequester`、`di-native-provider`、Qwen MiniNDN launcher；Selection 后的 source materialization 资源诊断。
- 原设计 / 新设计 / 修改原因：普通路径在 authenticated Selection/grant/placement 后通过 Repo 获取 root/source/material。为验证 Qwen3-0.6B 的内存边界，当前增加显式、默认关闭的 `cache-compatibility` diagnostic：requester 交付绑定 system-wide plain source-cache namespace 的 metadata-only receipt，不创建 run-scoped encrypted Repo；Provider 复核 source cache 的 identity schema、model/manifest digest、大小和 SHA-256 后，跳过该阶段的 Repo material fetch，由既有 native assembly/ONNX/lineage/runner 校验继续消费本地文件。
- 当前安全与契约边界：普通 authenticated prepare、manifest、ACK、Selection、授权和 placement 仍必须执行；material-backed source 不支持；若 protected role 已建立 `ProtectedRuntime`，显式 diagnostic 可使用 verified local source，但普通 protected Repo qualification 仍走 grant-bound encrypted path；缺缓存或任何校验不匹配 fail closed；不改变 ONNX input/KV/output、lineage wire 或正式 Repo contract。该模式只能作为诊断证据，不能满足 FR-003/FR-019 或 `QWEN_TWO_PROVIDER_PASS`。
- 当前已实现部分 / 目标未实现部分：requester publisher、provider assembler、CLI 参数传播和兼容模式 run-scoped Repo 抑制已完成；Python `py_compile`、静态分支审查、`ndnsf-distributed-inference,DI_NativeRequester,di-native-provider` 210-task build、两个 global install、CLI 和 `ldd` 检查均通过。旧 r163 runtime 在 ACK 前触发 `RESOURCE_BOUNDARY:diskFree`；r164 也在 host admission 阶段因 `diskFreeBytes=4232839168` 低于 4 GiB 门限停止，未进入 requester/Provider；两 Provider terminal/output/drain/repeat 与资格仍待完成。
- 源码与证据：`NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.{hpp,cpp}`、`examples/DI_NativeRequester.cpp`、`examples/DI_NativeProviderExecutable.cpp`、`Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py`；[static evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-r163-cache-compatibility-static-20260920.md)、[r163 disk evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-r163-disk-boundary-20260920.md)、[r164 disk evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-r164-requester-cache-disk-boundary-20260920.md)、[r162 stop evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-r162-cache-compatibility-stop-20260920.md)。
- 状态 / 剩余验收 / 下一步：`PARTIAL`；先获得足够磁盘空间，再用新的 run ID 运行该诊断，记录是否越过 materialization 以及实际 RSS/MemAvailable/ownedSwap；失败后先定位首个边界，再修复并重新执行静态审查。

### D-190-FILEBACKED-WORKER：assembly worker 使用 file-backed canonical source，并按调用链重排未启动任务

- 日期 / Spec / 任务与契约 ID：2026-09-22；[Spec190](../specs/190-multiturn-latency/spec.md)；T003；`Q190-ASSEMBLY`、`FR-014`。
- 模块 / 当前与目标章节：`NativeOnnxAssemblyWorker`、`NativeOnnxRecipeAssembler`、`NativeCanonicalOnnxAssembler`；Spec190 execution order。
- 原设计 / 新设计 / 修改原因：原生产 worker 通过父子管道接收 model-sized bytes，父进程还保留完整 source vector；当前 parent 传递只读 fd 3，worker 用 zero-copy protobuf stream 读取并校验长度/source identity，materialized-role 在 worker 前释放 source vector。该改变减少 pipe/父子 duplicate，不放宽 structural/digest/IO/wire fail-closed 校验。与此同时，未启动任务按生产调用链从固定 Repo owner、prepare lookup、protected material、resident session、stage transfer 到 FINALIZE/drain 重排。
- 当前已实现部分 / 目标未实现部分：file-backed request/frame/metadata、subprocess parity 和生产接线已通过受影响 compile-link 及精确 C++ selectors；run-38 的 Provider-0 完成 worker/cache finalization/`RUNNER_READY`，但 Provider-1 仍以 `RESOURCE_BOUNDARY:MemAvailable` 停止。normal encrypted Repo 的跨进程 protected assembled-cache 仍明确 miss，持久 hash/cache 命中未实现；T003 `PARTIAL`，T004 `NOT_STARTED`。
- 兼容性、迁移或撤回影响：in-process parity 入口保持旧内存路径；worker 请求 bit/metadata 扩展只由当前 production caller 生成；fd 不继承给无关子进程。撤回时恢复内联 model bytes，但不改变 runner、Repo、Selection 或授权契约。
- 源码与文档提交定位：`NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.{hpp,cpp}`、`NativeOnnxRecipeAssembler.{hpp,cpp}`、`NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.cpp`、`tests/unit-tests/di-native-onnx-recipe.t.cpp`、`tests/wscript`、`specs/190-multiturn-latency/{spec,plan,tasks}.md`；[T003 evidence](../specs/190-multiturn-latency/evidence/b190-03.md)。
- 验证与状态：窄目标 `115/115` compile-link；file-backed frame/metadata/subprocess selectors `1/1`，materialized-role exact selector `1/1`，cold gate `1/1`，readiness `15/15`；run-35/36/37 为 preflight boundary，run-38 `cleanup=PASS` 但 normal Repo `UNQUALIFIED`。旧全量 materialized-role suite 因既有超大负例未完成，记为 `UNOBSERVED`。
- 状态 / 剩余验收 / 下一步：`PARTIAL` / `OPEN_FOR_NEXT_BATCH`；当前只允许完成 T003。T003 完成后按新序列执行 `T004 RepoRestart → T005 RepoLookupReuse → T006 ProtectedMaterialReuse → T007 ResidentSession → T008 StageTransferBudget → T009 TerminalDrain → T010 Convergence → T011 MatchedExperiment`，不跳项、不把 persistent protected cache miss 当作正常命中。

### D-189-HASH-CACHE：assembled plaintext cache 的 hash-only admission — 2026-09-21

- **Status**: `PARTIAL` / internal native cache simplification。authenticated
  Selection/grant 通过后，Provider 在系统唯一的 `assembled/<role>/` 下扫描
  content-addressed entry，只读取 `model.onnx`，并以目录名中的 SHA-256 与文件流式
  SHA-256 相等作为命中条件；不再要求 `manifest.json` 或 `manifest.signature`。
- **Design boundary**: 这只简化本地已装配模型的完整性/复用检查；它不是 authenticity
  或新的授权边界。普通 protected Repo 路径仍保持 grant-bound encrypted staging；仅
  显式 cache-compatibility diagnostic 在既有 `ProtectedRuntime` 和 Selection/grant
  成功后允许 protected role 使用该 hash-only assembled cache，并跳过第二份 staging。
  `artifactDigest` 仍表示 placement/layer identity，不被误用为 assembled 文件摘要。
- **Source / evidence**: `NativeCanonicalOnnxAssembler.{hpp,cpp}`、`Provider.cpp`、
  `tests/integration-tests/di-prepared-provider.t.cpp`；[B189 assembled-model cache
  evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-assembled-model-cache-20260921.md)。
  同批还修正了 streaming SHA-256 size guard 的 unsigned-underflow 边界。
- **Validation boundary**: affected DI closure 已 build/install，`Spec185ProviderAssembly`
  19/19 focused cases 通过；这不证明真实 Qwen two-provider 的 assembly、handoff、terminal
  或 qualification。r211 的首个当前生产边界仍是 `NATIVE_REQUEST_TIMEOUT boundary=Core`。

### D-189-HASH-CACHE-REPAIR：显式 cache-compatibility 的 protected assembled 命中 — 2026-09-21

- **变更原因**：r229 证明原 hash-only assembled lookup 只接受 `plaintext-v1`，而显式
  cache-compatibility diagnostic 仍带 authenticated protected epoch，导致已组装模型未被
  复用并退回大模型 cold assembly；Provider 命中分支还必须跳过 encrypted artifact
  lifetime/path 读取。
- **当前契约**：Selection/grant/placement 和 `ProtectedRuntime` 仍是门槛；诊断模式才可
  扫描 `assembled/<safe-role>/<64-hex>/model.onnx`，重新计算 SHA-256 后复用；首次
  protected diagnostic assembly 将解密后的最终模型写入同一内容寻址目录。普通 protected
  Repo 仍只保留 grant-bound encrypted artifact，不跨 grant 复用该明文缓存；material-backed
  source 仍 fail closed。
- **验证与边界**：受影响 Waf build 与 `Spec185ProviderAssembly` 19/19 通过；r229 原始
  run 在 `RUNNER_PREPARATION_FACTORY_BEGIN` 有界停止，未产生 worker/runner/ORT/terminal，
  因此状态仍为 `PARTIAL`/`RUNTIME_UNQUALIFIED`。post-repair immutable snapshots 为
  `.codex-tmp/spec189-r229-cache-repair-static-review-v1/` 和 `...-v2/`；新的真实
  cache-hit runtime 与正式 protected Repo qualification 仍待执行。
# Spec189 r258 — Chained multi-turn KV (PARTIAL)

2026-09-22：逐轮checkpoint接续归MiniNDN配置编排；会话校验、Provider KV恢复和
finalization仍归原生C++。新增精确恢复观测区别于模型cache hit，C++ oracle扩展逐轮
检查。遵守G1–G6、D1–D4、B1–B3、M1–M3，不改变公开会话API或冻结目标。
当前尚未运行，双PDF及整体API交付保持PARTIAL；见
[r258](../specs/189-qwen-two-provider-minindn/evidence/b189-r258-multiturn-kv.md)。

### D-190-ASSEMBLY-WORKER-ORT：分离 assembly worker 校验与生产 runner 加载

- 日期 / Spec / 任务与契约 ID：2026-09-22；[Spec190](../specs/190-multiturn-latency/spec.md)；T003；`Q190-ASSEMBLY`。
- 模块 / 当前与目标章节：`NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.cpp`；assembly worker 与 Provider runner preparation。
- 原设计 / 新设计 / 修改原因：静态审查确认 worker 响应契约只要求 canonical source/recipe 的结构、digest、node coverage、boundary IO、wire 和响应字段校验；worker 内完整 ORT session 不会替代 parent/provider 随后的生产 runner 创建与 warmup，反而会与已驻留 runner 形成第二份 model-sized graph。当前 internal chain 增加 `loadRuntimeSession` seam：公共 `assembleNativeCertifiedOnnxModel` parity 路径保留 ORT load，生产 child `assembleInProcess` 跳过重复 ORT load，由 `OnnxRuntimeModelRunner` 在 `RUNNER_READY` 前承担唯一 authoritative load/warmup。
- 当前已实现部分 / 目标未实现部分：受影响 DI/worker/provider/test targets compile-link、安装及 focused C++ 回归通过；run-34 证明 Provider-0 可完成 worker、cache finalization、runner create 和 `RUNNER_READY`，但 Provider-1 仍在 model materialization/structural assembly 阶段触发 `RESOURCE_BOUNDARY:MemAvailable`。因此 T003 仍 `PARTIAL`；CD-04 的 resident session cache 仍是 planned target，不得反写为 current behavior。
- 兼容性、迁移或撤回影响：不改变 worker fail-closed structural/digest/IO/wire contract、Repo、ACK/Selection、ONNX input/KV/output 或公开 runner factory API；撤回该 gate 会恢复重复 cold ORT load，但不改变后续 authoritative runner load。未改变目标设计 PDF 或 planned CD-04。
- 源码与证据：`NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.cpp`；[Spec190 T003 evidence](../specs/190-multiturn-latency/evidence/b190-03.md)；[run-34 boundary](../specs/190-multiturn-latency/evidence/b190-03.md#run-34-normal-repo-result)。
- 验证与状态：Waf affected compile-link PASS（48.955s）；`spec190-materialized-role` 1/1、cold assembly selector 1/1、`spec190-readiness-progress` 15/15；normal Repo run-34 `UNQUALIFIED`，cleanup PASS，无 terminal/run-record/oracle 完整结果；T004 未启动。

### D-190-FILEBACKED-RESPONSE：file-backed assembly worker 的 digest-only response

- 日期 / Spec / 任务与契约 ID：2026-09-22；[Spec190](../specs/190-multiturn-latency/spec.md)；T003；`Q190-ASSEMBLY`、`FR-014`。
- 模块 / 当前与目标章节：`NativeOnnxAssemblyWorker`、`NativeOnnxRecipeAssembler`；file-backed worker parent/child response lifecycle。
- 原设计 / 新设计 / 修改原因：file-backed request 已避免 parent 通过 stdin 向 child 传入 model-sized bytes，但 child 原先仍把完整 assembled model 通过 stdout response 返回，造成 parent response buffer、worker result 和 finalizer input 的第二次重叠。当前 file-backed child 成功时返回 status `3` 的 digest-only frame；parent 在 reap 后从受限、已校验的 `modelFile` 读取最终 bytes，再复用原有 digest/recipe/contract finalizer。普通 inline request 保留 status `0` 的 model payload，未知 status 与不一致 frame 继续 fail-closed。
- 当前已实现部分 / 目标未实现部分：status-3 decoder/composer/child/parent 接线通过静态复审、`133/133` compile-link、focused C++ regression 和真实第一轮 two-provider execution；run-43 第二轮仍因 Provider-1 materialization/assembly 的 resident-runner overlap 触发 `RESOURCE_BOUNDARY:ownedSwap`。跨 request persistent assembled runner/cache reuse、三轮 terminal、C++ oracle 和完整资格仍未完成。
- 兼容性、迁移或撤回影响：只扩展现有 worker response status contract；status `0` inline compatibility 保持，生产 file-backed caller 才使用 status `3`。不改变 Repo、ACK/Selection/grant、authoritative ORT session、ONNX input/KV/output 或公开 runner API；撤回时恢复 file-backed response 的 model payload，不改变结构校验。
- 源码与证据：`NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.{hpp,cpp}`、`NativeOnnxRecipeAssembler.{hpp,cpp}`、`NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.cpp`、`tests/unit-tests/di-native-onnx-recipe.t.cpp`、`tests/wscript`；[T003 evidence](../specs/190-multiturn-latency/evidence/b190-03.md)。
- 验证与状态：`133/133` compile-link；worker protocol decoder/composer 3 selectors `1/1`、file-backed subprocess `1/1`、materialized-role `1/1`、cold gate `1/1`、readiness `15/15`；run-43 第一轮 terminal 成功，第二轮 `RESOURCE_BOUNDARY:ownedSwap`，cleanup PASS。状态 `PARTIAL`；T004 仍 `NOT_STARTED`。

### D-190-CACHE-DIR：prepare 显式绑定跨 run immutable cache root

- 日期 / Spec / 任务与契约 ID：2026-09-23；[Spec190](../specs/190-multiturn-latency/spec.md)；T003；`CD-07`、`CD-08`。
- 模块 / 当前与目标章节：`Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py`；LocalExperiment `prepare`/launch contract；native runner 已有 `--artifact-cache-root`。
- 原设计 / 新设计 / 修改原因：原 launcher 只依赖 native runner 的默认缓存根，prepare 记录中没有显式的跨 run cache destination。现在 `prepare` 接受 `--cache-dir`（兼容 `--artifact-cache-root`），记录绝对 `cacheRoot` 并显式传给 native runner。缓存根必须在 run evidence/workload 外；它只承载按完整 source identity/recipe digest 分区的 immutable source/provider cache，不接管 request、授权、encrypted Repo、日志或 KV 生命周期。
- 当前已实现部分 / 目标未实现部分：wrapper 参数传播、路径 fail-closed 检查、exact identity reuse/hash-verified object 测试完成；真实固定 Repo owner、重启后的 production `user.prepare` lookup/store 计数、protected material reuse 和两节点终态仍未实现/观测。
- 兼容性、迁移或撤回影响：默认值保持 native runner 原有 `/var/tmp/ndnsf-di-native-artifacts`；旧 prepared bundle 未强制新增字段，仍可按原 launch command 校验；撤回只移除 wrapper 参数，不改变 C++ source/assembled cache owner。
- 源码与证据：`Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py`、`Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py`、`tests/python/test_spec184_qwen06b_local_experiment.py`；[B190-05](../specs/190-multiturn-latency/evidence/b190-05.md)。
- 验证与状态：`py_compile` PASS；LocalExperiment 定向测试 `32 passed`；未启动新的 normal Repo run，状态 `PARTIAL`，T003 仍 `PARTIAL`，T004 仍 `NOT_STARTED`。本变更不是公开 C++ `User::prepare` 签名或 Core API 变化，因此不改写 Core API reference/PDF。

### D-190-INT8-SOURCE-COMPATIBILITY：预构建 weight-only INT8 source compatibility gate

- 日期 / Spec / 任务与契约 ID：2026-09-23；[Spec190](../specs/190-multiturn-latency/spec.md)；T003/B190-03A；`CD-INT8`、`FR-007`、`FR-014`。
- 模块 / 当前与目标章节：`NDNSF-DistributedInference` canonical catalog/publication/assembler/ORT runner；Spec190 model-source compatibility recovery gate。
- 原设计 / 新设计 / 修改原因：仅替换原始 ONNX 的 FP16/INT8 表示同时影响 source identity、initializer shape/digest、材料分段、Repo publication、assembled cache 和 runner contract。当前直接候选的公开 IO/KV 已通过 ORT CPU smoke，但 r6 在 inline initializer 超过 1 MiB material bundle 上限处停止。新增受限目标：支持固定摘要的 `ONNX + weight_only_int8` 候选，其 public control/activation/KV/output 仍为现有 `INT64/FP32` contract；全图 INT8、signed-INT8 wire 和量化导出仍排除。
- 当前已实现部分 / 目标未实现部分：固定缓存下载、候选静态统计、Python wire-shape 修正和 ORT one-token smoke 已完成；C++ quantization subtype descriptor、inline bounded chunk/reassembly、ownership ledger、direct candidate native gate 和 two-provider chain 尚未实现/验证。r6 没有 ACK、Selection、assembly、terminal 或 qualification 结果。
- 兼容性、迁移或撤回影响：不得提高 1 MiB payload 上限，不得把 FP32 public precision 改名为 all-INT8，不得因 cache hit 绕过 grant/ACK/Selection/placement。只有真实 C++ IO/KV mismatch 才允许增加独立 adapter；若失败，候选保持 `UNQUALIFIED`，T003 不解锁 T004。
- 源码与证据：`NativeCanonicalArtifactPublisher.cpp`、`NativeCanonicalOnnxAssembler.cpp`、`NativePlanning.*`、Python ONNX graph adapter；[Spec190 audit](../specs/190-multiturn-latency/audit.md#2026-09-23-direct-int8-source-compatibility-audit)、[r6 evidence](../specs/190-multiturn-latency/evidence/b190-03.md#direct-int8-candidate-downloaded-and-r6-preparation-boundary-2026-09-23)。
- 验证与状态：candidate ORT CPU one-token smoke PASS；direct production r6 `PREPARATION_FAILED / DI_NATIVE_PUBLICATION_MATERIAL_PAYLOAD_TOO_LARGE`，cleanup PASS；本轮为全局静态审计和计划修订，状态 `PARTIAL`，T004 仍 `NOT_STARTED`。尚无公开 C++ API 变更或 PDF 实现快照更新。

### D-190-INT8-IDENTITY-MATERIAL-REPAIR：量化 subtype 身份与 inline material bounded chunks — 2026-09-23

- 日期 / Spec / 任务与契约 ID：2026-09-23；[Spec190](../specs/190-multiturn-latency/spec.md)；T003/B190-03A；`CD-INT8`、`FR-007`、`FR-014`。
- 模块 / 当前与目标章节：`NativePlanning` model identity；`NativeOnnxRecipeAssembler` canonical material publication/reassembly；Python candidate manifest normalization。
- 原设计 / 新设计 / 修改原因：旧 descriptor canonical JSON 不区分 `none` 与 `weight_only_int8`，且 inline `raw_data` 直接形成超过 1 MiB material payload。现在非默认 quantization subtype 进入 model identity，legacy `none` 保持旧 canonical JSON；大 inline initializer 移入共享 backing 并按现有 bounded chunk contract 发布、校验和重组，不提高 payload 上限。
- 当前已实现部分 / 目标未实现部分：C++ identity selector `6/6`、inline bounded-chunk selector `25/25`、external parity selector `26/26` 和受影响 targets compile-link 已通过；direct candidate native assembler/ORT continuation、normal Repo、ACK/Selection、两 Provider terminal 与完整 T003 仍未完成。fixture dataflow 在 `NativeExecutionPlanJson.cpp:1413` 停止，最新 direct r7 在 root-owner preflight 停止。
- 兼容性、迁移或撤回影响：仅允许 `none` 与 `weight_only_int8`；不改变 public FP32 activation/KV/output contract，不绕过 grant/ACK/Selection/placement，不把 focused gates 计为 qualification。撤回时可移除 subtype 解析和 inline backing path，但须同时移除对应 tests 与 evidence update。
- 源码与证据：`NativePlanning.*`、`NativeRequestCatalog.cpp`、`NativeCanonicalRolePreparer.cpp`、`NativeOnnxRecipeAssembler.*`、`Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py`；[T003 evidence](../specs/190-multiturn-latency/evidence/b190-03.md)；[failure log](../docs/failure-log.md)。
- 验证与状态：system-first Waf affected targets compile-link PASS；named C++ selectors `6/6`、`25/25`、`26/26` PASS；Python syntax PASS；native fixture is `FAIL_FIXTURE_DATAFLOW` and direct r7 is `FAIL_PRECHECK` because root owner is unavailable. 状态仍 `PARTIAL`，T004 仍 `NOT_STARTED`；current API text updated, PDF implementation snapshot not rebuilt in this unit。

### D-190-TOKEN-BUDGET-1025：normal launcher token budget correction — 2026-09-23

- 日期 / Spec / 任务与契约 ID：2026-09-23；[Spec190](../specs/190-multiturn-latency/spec.md)；T003；`CD-02`、`SC-001`。
- 模块 / 当前与目标章节：`Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py`、`Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py`；normal-run workload contract。
- 原设计 / 新设计 / 修改原因：当前验收要求每轮最多 `1025` 个新 token，并以 EOS 或预算结束；wrapper 原来把上限错误限制为 `64`，native runner 原来限制为 `1024`，导致有效验收参数在 prepare/native preflight 前被拒绝。两层现在统一接受 `1..1025`；本次不改变采样、EOS、KV、Repo 或资源门。
- 当前已实现部分 / 目标未实现部分：两层 validation、command forwarding regression 和 launcher syntax 已通过；r11 尚未完成，因此真实三轮 EOS/terminal/性能仍未观测。
- 兼容性、迁移或撤回影响：低于 1025 的旧命令仍合法；超过 1025 继续 fail-closed。撤回需同时恢复两层上限、测试和 Spec190 参数记录；不涉及公开 C++ API 或安装 ABI。
- 源码与证据：`Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py`、`Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py`、`tests/python/test_spec184_qwen06b_local_experiment.py`；[B190-03](../specs/190-multiturn-latency/evidence/b190-03.md#normal-launcher-pty-path-and-token-budget-correction-2026-09-23)。
- 验证与状态：`33 passed`、两 launcher `py_compile`、`git diff --check`；r10 使用错误的 `2` token budget 且未产生可用终态，r11 待重新 prepare/run。状态 `PARTIAL`，T003 仍 `PARTIAL`，T004 仍 `NOT_STARTED`。

### D-190-INSTALL-CLOSURE-QUANTIZATION：native requester 安装闭包修复 — 2026-09-23

- 日期 / Spec / 任务与契约 ID：2026-09-23；[Spec190](../specs/190-multiturn-latency/spec.md)；T003；`CD-INT8`、`FR-007`。
- 模块 / 当前与目标章节：`NativePlanning` model descriptor canonicalization；normal Repo requester 的 source/build/install closure。
- 原设计 / 新设计 / 修改原因：r11 使用当前 `quantization_subtype=weight_only_int8` 配置，但旧安装的 `DI_NativeRequester` 不认识该字段，在 preparation 以 unknown/lossy descriptor 停止。未放宽生产校验；按当前源码重新构建并安装 `ndnsf-distributed-inference` 与 requester，使运行时二进制与源码契约一致。
- 当前已实现部分 / 目标未实现部分：受影响目标 `119/119` 编译通过，安装后 requester 与 build artifact hash 一致，`ModelIdentityBindsWeightOnlyQuantizationSubtype` 通过 `1/1`、`6/6 assertions`。r11 仍未到 Repo/ACK/Selection/assembly/terminal；r12 尚未运行。
- 兼容性、迁移或撤回影响：这是本地安装闭包修复，不改变 wire/API、量化 subtype 规则、授权、Selection 或 cache 语义；回退必须恢复匹配的旧源码与二进制，不能只替换单个 requester。
- 源码与证据：`NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.cpp`、`build-spec189-oracle`、`/usr/local/bin/DI_NativeRequester`；[B190-03](../specs/190-multiturn-latency/evidence/b190-03.md#r11-preparation-boundary-and-installed-requester-closure-repair-2026-09-23)。
- 验证与状态：system-first Waf `119/119` build/install PASS；named identity selector PASS；Waf editable Python binding phase 仍报告 `NDNSF_GLOBAL_NATIVE_DIGESTS`，不计 native runtime PASS。r12 normal Repo chain 待执行；状态 `PARTIAL`，T003 仍 `PARTIAL`，T004 仍 `NOT_STARTED`。

### D-190-SERIALIZED-MATERIAL-BOUNDARY：inline material boundary repair — 2026-09-23

- 日期 / Spec / 任务与契约 ID：2026-09-23；[Spec190](../specs/190-multiturn-latency/spec.md)；T003；`CD-INT8`、`FR-014`。
- 模块 / 当前与目标章节：`NativeOnnxRecipeAssembler` material manifest production and validation。
- 原设计 / 新设计 / 修改原因：Qwen INT8 有 initializer 的 `raw_data` 恰好为 `1 MiB`，但序列化 TensorProto 为 `1,048,617` 字节；旧实现只按 raw size 选择 inline branch，并在验证处再次只按 raw size 判断，造成边界 payload 超过 1 MiB 上限后失败。现在两处均把 raw size 和 serialized TensorProto size 纳入 branch/validation 条件，边界对象转为 bounded chunks。
- 当前已实现部分 / 目标未实现部分：生产与验证条件已统一；新增 exact-boundary C++ regression，连同现有 large-inline regression 通过 `2/2`、`31/31 assertions`。固定库尚未安装，r12 后续完整 Repo 链路尚未重跑。
- 兼容性、迁移或撤回影响：不提高 material payload cap，不改变 digest、授权、Selection、backend 或 public FP32/KV/output contract；仅修复已支持的 bounded chunk representation 在精确边界上的遗漏。
- 源码与证据：`NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.cpp`、`tests/unit-tests/di-native-canonical-publisher.t.cpp`；[B190-03](../specs/190-multiturn-latency/evidence/b190-03.md#r12-preparation-boundary-and-serialized-size-boundary-repair-2026-09-23)。
- 验证与状态：affected native targets compile/link PASS；`Spec182CanonicalPublisher/InlineInitializer*` PASS；尚未 install 或重跑 normal Repo，状态 `PARTIAL`，T003 仍 `PARTIAL`，T004 仍 `NOT_STARTED`。

### D-190-SHARED-TOKEN-LIMIT：统一 native token budget boundary — 2026-09-23

- 日期 / Spec / 任务与契约 ID：2026-09-23；[Spec190](../specs/190-multiturn-latency/spec.md)；T003；`CD-02`、`SC-001`。
- 模块 / 当前与目标章节：`NDNSF-DistributedInference/cpp/ndnsf-di/NativeGenerationLimits.hpp`；`NativeRequestEnvelope`、projection/role contract；Spec190 normal launcher。
- 原设计 / 新设计 / 修改原因：r13 证明 wrapper/native CLI 的 `1025` 上限与共享 C++ envelope 的 `1024` 不一致，request 在真实 User.request 阶段被拒绝。共享 C++ 常量提升为 `1025`；边界回归同时要求 `1025` 通过、`1026` 失败，保留所有其他 generation/options 校验。
- 当前已实现部分 / 目标未实现部分：代码和 C++ selector 修改已写入，尚未完成本次受影响目标编译、安装和 fresh normal Repo 重跑；ACK/Selection/placement/assembly/terminal/EOS/three-turn 仍未验证。
- 兼容性、迁移或撤回影响：`1..1025` 合法，`0` 和 `>1025` 仍 fail-closed；不改变 ACK window、EOS、KV、Repo、授权或资源策略。撤回需同步恢复共享常量及边界 selector，不能只回退 wrapper。
- 源码与证据：`NativeGenerationLimits.hpp`、`tests/unit-tests/distributed-inference-native-plan.t.cpp`；[r13 Changed gate](../specs/190-multiturn-latency/evidence/b190-03.md#r13-request-contract-boundary-and-shared-token-limit-changed-gate-2026-09-23)。
- 验证与状态：受影响 `spec189-epoch-projection` compile/link PASS；`NativeGenerationBudgetAccepts1025AndRetainsWireBounds` 为 `1 test / 22 assertions PASS`；安装库与 build library hash 均为 `ed36d1d0260fe5098fc46dc5cfbab7801172e9881ee81e9e0550bddf6ffb8fb5`，安装头文件为 `1025`。r14 越过 request contract 后暴露新的 Qwen state-name boundary，T003 继续 `PARTIAL`，T004 仍 `NOT_STARTED`。

### D-190-QWEN-CANONICAL-STATE-NAMES：Qwen semantic KV 到 ONNX source 名称绑定 — 2026-09-23

- 日期 / Spec / 任务与契约 ID：2026-09-23；[Spec190](../specs/190-multiturn-latency/spec.md)；T003；`CD-INT8`、`FR-007`、`FR-014`。
- 模块 / 当前与目标章节：`Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py` 的 stage/catalog/options 生成；`NativeCanonicalRolePreparer::bindStateContracts` 的 source-boundary 校验；Qwen ONNX state/KV contract。
- 原设计 / 新设计 / 修改原因：semantic conversation vocabulary `past_key.N` 等不等于该 Qwen export 的 ONNX source names。r14 证明 identity mapping 在 ACK 后由 C++ 正确拒绝；修复只在 Qwen profile 生成 authenticated mapping，将 inputs 映射为 `past_key_values.N.key/value`、outputs 映射为 `present.N.key/value`，并让 runner metadata、generation options 与 successor map 使用同一 canonical names。保留 C++ source-boundary fail-closed 校验，不增加 provider adapter、不放宽 state contract。
- 当前已实现部分 / 目标未实现部分：r14 首边界已静态定位并保留；mapping 修复、C++/Python 回归、fresh normal Repo 和三轮 EOS/KV/cleanup 尚未完成，T003 继续 `PARTIAL`。
- 兼容性、迁移或撤回影响：只影响 Qwen native ONNX profile；generic fixtures 和 semantic API vocabulary 不改变。撤回需同步移除 mapping helper、metadata/options wiring 及对应 positive/negative tests；不得恢复把 semantic 名称直接当 source name 的行为。
- 源码与证据：`Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py`、`NativeCanonicalRolePreparer.cpp`、`tests/unit-tests/di-native-canonical-publisher.t.cpp`、`tests/python/test_spec184_qwen06b_local_experiment.py`；[r14 evidence](../specs/190-multiturn-latency/evidence/b190-03.md#r14-post-ack-state-contract-boundary-and-qwen-canonical-name-changed-gate-2026-09-23)。
- 验证与状态：`py_compile PASS`；Qwen launcher 定向回归 `39 passed`；已有 C++ `StateBindingConsumesActualCausalOnnxExport` 为 `1 test / 29 assertions PASS`；production `stage_plan_and_manifest` 输出 canonical input/output/successor names。未改 C++ source，故复用已安装 binder target 进行 focused source-bound regression；完整 Repo/Selection/assembly/terminal/EOS 仍未观测。当前 gate `static=PASS`、`compile-link=PASS`（existing target）、`runtime-test=PASS`、`unobserved=full normal chain`，Closure decision `OPEN_FOR_NEXT_BATCH`，T003 `PARTIAL`，T004 `NOT_STARTED`。

### D-190-STARTUP-COMPATIBILITY-CACHE：启动前兼容性摘要与重复校验收敛 — 2026-09-23

- 日期 / Spec / 任务与契约 ID：2026-09-23；[Spec190](../specs/190-multiturn-latency/spec.md)；T003；`CD-INT8`、`FR-007`、`FR-014`。
- 模块 / 当前与目标章节：`Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py` 的 candidate preflight；`Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py` 的 pre-materialization identity gate。
- 原设计 / 新设计 / 修改原因：启动器原本每次都重新解析同一个大型 canonical ONNX，且 native runner 的首次 identity gate 内重复执行 receipt/binary digest loop。现在 outer preflight 按 canonical source SHA-256 持久缓存仅 graph summary；candidate identity 显式记录 model format、quantization 与 quantization subtype；native runner 保留首次检查和 MiniNDN 前 TOCTOU fence，只删除同一检查块的重复循环。
- 当前已实现部分 / 目标未实现部分：cache hit/negative isolation、identity normalization、旧重复校验删除和 real candidate `check` 已通过；fixed C++ library 尚未安装，Repo/ACK/Selection/placement/assembly/terminal/EOS/three-turn 仍未验证，驻留 runner/Repo reuse 仍为后续任务。
- 兼容性、迁移或撤回影响：cache 不保存或复制模型字节，不改变 material cap、source/initializer/binary digest、grant、ACK、Selection、placement、cleanup 或 native runner contract；不同 source digest 使用不同 summary namespace。撤回只移除 summary cache 和 candidate identity 加字段，保留两处必要 identity checks。
- 源码与证据：`Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py`、`Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py`、`tests/python/test_spec184_qwen06b_local_experiment.py`；[B190-03 startup Changed gate](../specs/190-multiturn-latency/evidence/b190-03.md#startup-simplification-changed-gate-2026-09-23)。
- 验证与状态：Spec Kit entrypoint `11/11 PASS`；两个 launcher `py_compile PASS`；Python launcher regression `35 passed`；r12-input outer check `PASS`。本单元没有 native compile/link 或 MiniNDN run；T003 继续 `PARTIAL`，T004 仍 `NOT_STARTED`。

### D-190-PROTECTED-REUSE-CONTRACT：protected durable reuse target contract freeze — 2026-09-23

- 日期 / Spec / 任务与契约 ID：2026-09-23；[Spec190](../specs/190-multiturn-latency/spec.md)；T006/B190-11；`CD-09`、`FR-017`、`SC-007`、`SC-008`、`SC-009`。
- 模块 / 当前与目标章节：`ServiceUser` encrypted large-data publication、`RepoEncryptedLargeDataStore`、`NativeCanonicalOnnxAssembler` protected cache、Provider serving lease；`contracts/material-reuse.md#cd-09-protected-material-reuse-boundary`。
- 原设计 / 新设计 / 修改原因：当前 protected 路径仍强制 miss，Repo `Source` 析构仍按 transient lease 删除对象，Core wrapped send key 仍是进程态；本次只冻结 target contract，不提前修改生产代码。目标明确区分 `Transient/Durable` retention，要求 ciphertext-only Repo backing、Core-owned opaque key-reference recovery、serving re-registration、当前新 grant/Selection/placement 重新绑定和显式失效 GC。
- 当前已实现部分 / 目标未实现部分：五 lane static review 已确认首个 owner/key-recovery 边界；target interface 已写入 CD-09。durable key-reference persistence/recovery、稳定 protected identity、Repo durable source、Provider protected hit、assembled cache 解锁和 C++ regression 尚未实现，T006 保持 `IN_PROGRESS`。
- 兼容性、迁移或撤回影响：现有无选项 `commitFile` 继续表示 transient；不存储 plaintext、private key、完整 grant、旧 request 或 KV；requestId/attempt/provider boot 不能成为 durable identity。任一 key/reference/serving/grant 检查失败必须 typed miss/rejection，不能把 compatibility source 或保留文件当作 hit。撤回只删除本 target contract 与对应 T006 evidence，不改变 current behavior。
- 源码与证据：`ndn-service-framework/EncryptedLargeDataRangeStore.hpp`、`ndn-service-framework/ServiceUser.cpp`、`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoEncryptedLargeDataStore.hpp`、`NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.cpp`；[B190-11](../specs/190-multiturn-latency/evidence/b190-11.md)；[CD-09](../specs/190-multiturn-latency/contracts/material-reuse.md#cd-09-protected-material-reuse-boundary)。
- 验证与状态：Context Mode project/active health、Spec Kit entrypoint sync `11/11 PASS`、Spec structure audit `PASS`、`git diff --check` PASS；没有 native compile/link、runtime-test 或真实 Qwen run。当前状态 `PARTIAL/OPEN_FOR_NEXT_BATCH`，T007 继续锁定。

### D-190-PROTECTED-RETENTION-API：transient/durable encrypted Repo source lease — 2026-09-23

- 日期 / Spec / 任务与契约 ID：2026-09-23；[Spec190](../specs/190-multiturn-latency/spec.md)；T006/B190-12；`CD-09`、`FR-017`、`SC-007`。
- 模块 / 当前与目标章节：`ndn-service-framework/EncryptedLargeDataRangeStore.hpp`、`RepoEncryptedLargeDataStore.hpp` 的 source retention 与 explicit release；[CD-09 target interface](../specs/190-multiturn-latency/contracts/material-reuse.md#t006-target-interface-freeze-not-current-behavior)。
- 原设计 / 新设计 / 修改原因：旧 `commitFile` 继续保持 transient；新增明确的 `Transient/Durable` options overload。仅 Repo adapter 支持 durable，旧 adapter 收到 durable 必须 fail-closed；durable Source 析构不删除 committed object，owner 可通过 idempotent generation-fenced `release()` 显式失效。该修复只关闭存储生命周期边界，不引入 key persistence 或 protected hit。
- 当前已实现部分 / 目标未实现部分：retention enum/options、Source `isDurable/release`、Repo adapter 和 C++ selector 已实现；Core crypto-owner key-reference recovery、稳定 protected identity、new-grant rebinding、Provider serving/assembled hit 和默认 protected 路径接线仍未实现，T006 继续 `IN_PROGRESS`。
- 兼容性、迁移或撤回影响：legacy transient callers 与 Spec189 cleanup 保持兼容；不存储 plaintext/private key/grant/KV，不改变 assembler protected miss。撤回需同步移除 overload、Repo retention branch、selector 与 B190-12，不得只删测试。
- 源码与证据：`ndn-service-framework/EncryptedLargeDataRangeStore.hpp`、`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoEncryptedLargeDataStore.hpp`、`tests/unit-tests/spec190-protected-material-reuse.t.cpp`、`tests/wscript`；[B190-12](../specs/190-multiturn-latency/evidence/b190-12.md)。
- 验证与状态：root `build/` configure PASS；new target `43/43` compile-link，selector `5 cases × 3` PASS；existing `spec189-encrypted-repo` `120/120` compile-link，6 cases PASS；`git diff --check` PASS。状态 `PARTIAL/OPEN_FOR_NEXT_BATCH`，key-reference/restart/real Qwen remains unobserved，T007 `NOT_STARTED`。

### D-190-PROTECTED-KEY-REFERENCE：authenticated protected key-reference identity gate — 2026-09-23

- 日期 / Spec / 任务与契约 ID：2026-09-23；[Spec190](../specs/190-multiturn-latency/spec.md)；T006/B190-13；`CD-09`、`FR-017`、`SC-007`、`SC-008`。
- 模块 / 当前与目标章节：`NativeGrantVerifier`、`ProtectedRuntime`、`NativeProtectedArtifactStore`、`NativeCanonicalOnnxAssembler` protected material identity。
- 原设计 / 新设计 / 修改原因：grant wire 已包含 `keyId`，但当前 verifier result 丢弃它，runtime 只持有 secret content key，assembled ciphertext context 不包含可重启复用的 key-reference。冻结由 authority/provider/model/epoch/keyId 派生的非秘密 `sha256:` reference，并将其纳入 protected ciphertext AAD/manifest；request、attempt、grant digest、provider boot、fencing 和 KV 仍为运行时绑定，不能进入 durable identity。
- 当前已实现部分 / 目标未实现部分：本条只记录静态审查和最小实现门；reference propagation、AAD/manifest binding、wrong-reference selector、稳定 protected cache lookup、当前新 grant 绑定和真实重启仍未完成。Assembler protected miss 不变。
- 兼容性、迁移或撤回影响：只扩展 protected assembled-entry identity；现有 plaintext/compatibility cache、transient Repo API 和 B190-12 durable source gate 不改变。缺失/错误 reference 必须 fail-closed，不得把旧 ciphertext 当命中。
- 源码与证据：`NativeGrantVerifier.*`、`ProtectedRuntime.*`、`NativeProtectedArtifactStore.*`、`NativeCanonicalOnnxAssembler.cpp`；[B190-13](../specs/190-multiturn-latency/evidence/b190-13.md)；[CD-09](../specs/190-multiturn-latency/contracts/material-reuse.md#cd-09-protected-material-reuse-boundary)。
- 验证与状态：本轮 static review 已完成；compile-link/runtime-test 尚未执行，ASan/UBSan 按用户范围 deferred。状态 `PARTIAL/OPEN_FOR_NEXT_BATCH`，T006 未完成，T007 继续锁定。

### D-190-PROTECTED-KEY-REFERENCE-IMPLEMENTED：key-reference propagation and authenticated artifact context — 2026-09-23

- 日期 / Spec / 任务与契约 ID：2026-09-23；[Spec190](../specs/190-multiturn-latency/spec.md)；T006/B190-14；`CD-09`、`FR-017`、`SC-007`、`SC-008`。
- 模块 / 当前与目标章节：`NativeGrantVerifier`、`ProtectedRuntime`、`NativeProtectedArtifactStore`、`NativeCanonicalOnnxAssembler`、Provider protected serving。
- 原设计 / 新设计 / 修改原因：原实现验证 grant 后丢弃 `keyId`，protected assembled context 只能绑定模型/recipe/storage，无法证明 ciphertext 属于当前授权 key identity。现在保存并校验由 authority/provider/model/epoch/keyId 派生的非秘密 `sha256:` reference，并将其加入 protected AAD/manifest；Assembler 和 Provider 缺少 reference 时 fail-closed。
- 当前已实现部分 / 目标未实现部分：B190-14 的 reference propagation、authorized accessor、AAD/manifest binding、wrong-reference C++ regression 已实现并通过；stable protected cache path、ciphertext-only durable Repo recovery、new-grant/Selection rebinding、跨进程 restart 和真实 Qwen 仍未实现或观测，protected miss 不变。
- 兼容性、迁移或撤回影响：只影响 protected assembled-entry identity；plaintext/compatibility cache、transient Repo API 和 B190-12 durable Source gate 不变。reference 不包含 content key、private key、grant wire、request/KV 或 provider boot/fencing；撤回必须同步移除 production field、AAD/manifest serialization、Assembler/Provider checks 与 B190-14 selector。
- 源码与证据：`NativeGrantVerifier.*`、`ProtectedRuntime.*`、`NativeProtectedArtifactStore.*`、`NativeCanonicalOnnxAssembler.cpp`、`Provider.cpp`；[B190-14](../specs/190-multiturn-latency/evidence/b190-14.md)。
- 验证与状态：普通根 `build/` `spec181-protected-runtime-closure` compile/link PASS（`-j2`，8m31.186s）；selector `32` C++ cases PASS；`git diff --check` PASS。ASan/UBSan 按用户范围 deferred；T006 仍 `PARTIAL`，T007 继续锁定。

### D-190-PROTECTED-DURABLE-METADATA：Core-owned protected identity in Repo manifest — 2026-09-23

- 日期 / Spec / 任务与契约 ID：2026-09-23；[Spec190](../specs/190-multiturn-latency/spec.md)；T006/B190-16；`CD-09`、`FR-017`、`SC-007`。
- 模块 / 当前与目标章节：`EncryptedLargeDataCommitOptions`、`RepoObjectManifest`、`RepoEncryptedLargeDataStore` 的 ciphertext-only durable publication metadata；[CD-09](../specs/190-multiturn-latency/contracts/material-reuse.md#cd-09-protected-material-reuse-boundary)。
- 原设计 / 新设计 / 修改原因：B190-15 确认稳定 protected path 不能先于 Core key-reference/serving owner。现在 Durable commit options 携带由 Core/crypto owner 提供的 publication identity、protection epoch、opaque key-reference id/version、ciphertext/encryption-manifest digest 和 serving locator；Repo 将这些非秘密字段写入可重启读取的 manifest，不生成、不解包密钥，也不把它们当授权结果。
- 当前已实现部分 / 目标未实现部分：Repo manifest 序列化/解析、Durable 缺字段 fail-closed、close/reopen metadata/read C++ regression 已实现；ServiceUser 的真实 key-reference recovery/serving re-registration、新 grant/Selection 绑定、稳定 protected assembled lookup、Provider hit 和真实 Qwen restart 仍未实现，protected miss 不变。
- 兼容性、迁移或撤回影响：无选项 legacy `commitFile` 继续是 Transient；不完整 Durable metadata 返回 `DURABLE_METADATA_INVALID`，不产生对象；旧 manifest 缺字段按空值读取并不伪装成 protected hit。撤回需同步移除 options/manifest 字段、JSON 兼容解析和 B190-16 selector，不能仅删除文档。
- 源码与证据：`ndn-service-framework/EncryptedLargeDataRangeStore.hpp`、`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoTypes.hpp`、`RepoTypes.cpp`、`RepoProtocol.cpp`、`RepoEncryptedLargeDataStore.hpp`、`tests/unit-tests/spec190-protected-material-reuse.t.cpp`；[B190-16](../specs/190-multiturn-latency/evidence/b190-16.md)。
- 验证与状态：普通根 `build/` 的 `spec190-protected-material-reuse` `43/43` compile-link，7 C++ cases 首轮通过并连续 3 次通过；公共头变更后的 `spec189-encrypted-repo` `120/120` compile-link，6 cases PASS；API reference 增量刷新成功，`git diff --check` PASS。ASan/UBSan deferred；此单元为 `ADVANCE/PARTIAL`，T006 与 protected miss gate 仍未闭合。

### D-190-PROTECTED-DURABLE-LOOKUP-SERVE：Core lookup-to-serving reuse — 2026-09-23

- 日期 / Spec / 任务与契约 ID：2026-09-23；[Spec190](../specs/190-multiturn-latency/spec.md)；T006/B190-17；`CD-09`、`FR-017`、`SC-007`。
- 模块 / 当前与目标章节：`EncryptedLargeDataRangeStore::lookupDurable`、`RepoEncryptedLargeDataStore`、`ServiceUser::publishEncryptedLargeDataImpl`、`NativeCanonicalArtifactPublisher`；[CD-09](../specs/190-multiturn-latency/contracts/material-reuse.md#cd-09-protected-material-reuse-boundary)。
- 原设计 / 新设计 / 修改原因：原 durable metadata 只有写入与重启读取，Core 仍会重新准备并提交同一大对象。现在 Core 在 durable publish 前按稳定 identity 查询，校验当前明文摘要/大小、当前 key epoch/reference、稳定 encrypted name、完整 reference metadata 与首段可读性，命中后直接登记 durable range source，避免 key generation、encryption 和第二次大 payload commit；当前 key/reference 不匹配时 fail closed。
- 当前已实现部分 / 目标未实现部分：Repo durable lookup、Core 同进程 serving re-registration、DI durable publisher 接线及 C++ 重复发布回归已实现；跨进程 key/reference recovery、新 grant/Selection rebinding、Provider/assembled hot hit、缺对象精确 fetch 和真实 Qwen restart 仍未实现或观测，normal protected miss 不变。
- 兼容性、迁移或撤回影响：legacy transient overload 与 request-scoped cleanup 不变；lookup miss 继续走原冷发布，metadata/source 不匹配 fail closed。撤回需同步移除 lookup API、Core hit 分支、DI durable transport 和 B190-17 回归，不能只删除文档。
- 源码与证据：`ndn-service-framework/EncryptedLargeDataRangeStore.hpp`、`ndn-service-framework/ServiceUser.cpp`、`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoEncryptedLargeDataStore.hpp`、`NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalArtifactPublisher.cpp`、`tests/integration-tests/spec189-encrypted-repo-publication.t.cpp`；[B190-17](../specs/190-multiturn-latency/evidence/b190-17.md)。
- 验证与状态：普通根 `build/` 的 Spec188/189/190 受影响目标构建成功；Spec190 7 cases、Spec189 7 cases、Spec188 5 cases PASS，restart lookup 与 durable serving hit 重复 3 次 PASS；`git diff --check` PASS。ASan/UBSan deferred；此单元为 `ADVANCE/PARTIAL`，T006 与 protected miss gate 仍未闭合。

### D-190-POLICY-TRANSITION-PROTECTED-FENCE：ControllerVersion 与 durable protected serving fence — 2026-09-23

- 日期 / Spec / 任务与契约 ID：2026-09-23；[Spec190](../specs/190-multiturn-latency/spec.md)；T006/B190-20；`CD-09`、`FR-017`、`SC-007`、`SC-008`。
- 模块 / 当前与目标章节：`ServiceUser::installControllerStatus`、`retireProtectedPublications`、`publishEncryptedLargeDataImpl`、protected IMS/file serving；[CD-09](../specs/190-multiturn-latency/contracts/material-reuse.md#cd-09-protected-material-reuse-boundary)。
- 原设计 / 新设计 / 修改原因：B190-19 确认仅删除 reference 不能撤销已经注册的 serving owner，且旧 publication 可与 policy transition 并发完成。本轮将 `ControllerVersion` 绑定到 service-scoped publication/IMS owner/reference，使用 pending publication 与最终 reference commit fence，并把 protected response fence 延续到 `m_face.put`；版本推进只撤销受影响 service，保留 Repo ciphertext。
- 当前已实现部分 / 目标未实现部分：B190-20 的旧 owner retirement、unaffected-service reuse、same-version restore、paused-publication stale negative、V2 fail-closed/rebind 和 C++回归已实现；真实 OS restart/decrypt serving、在线 Controller confirmation、grant/Selection/placement rebinding、Provider/assembled hit、精确 missing-object fetch 和真实 Qwen/MiniNDN 仍未实现或观测，protected assembler miss 保持，T006 `PARTIAL`，T007 锁定。
- 兼容性、迁移或撤回影响：旧 transient API 与 Repo durable ciphertext 保留语义不变；只影响 protected durable publication 的 owner/reference 生命周期。撤回必须同步移除 `ServiceUser` fence、V2 reference semantics、pending owner bookkeeping 和 B190-20 native oracle，不得只删除测试或文档。
- 源码与证据：`ndn-service-framework/ServiceUser.hpp`、`ndn-service-framework/ServiceUser.cpp`、`tests/integration-tests/spec189-encrypted-repo-publication.t.cpp`；[B190-20](../specs/190-multiturn-latency/evidence/b190-20.md)。
- 验证与状态：只读复核 `SAFE`/P1=0；普通根 `build/` 的 `spec189-encrypted-repo` `120/120` compile-link；stale-publication selector、完整 10-case suite 和完整 suite 三次重复均 PASS；ASan/UBSan deferred。当前状态 `ADVANCE/PARTIAL`，T006 未闭合，T007 不解锁。

### D-190-TEST-EXACT-GRANT-MISSING-NEGATIVE：Face-backed wrong-name regression — 2026-09-23

- 日期 / Spec / 任务与契约 ID：2026-09-23；[Spec190](../specs/190-multiturn-latency/spec.md)；T006/B190-29。
- 模块 / 当前与目标章节：served-Provider C++ integration fixture；不改变 Core、NDNSF-DI、Repo 或公开 API 契约。
- 原设计 / 新设计 / 修改原因：B190-28 已覆盖 exact signed grant 的成功往返，本轮在同一默认 factory/Face-backed callback 上加入错误 grant name 的 fail-closed 负例，避免测试只证明 happy path。首次负例运行暴露的是测试 callback moved-from 使用，已用同一 callback 的 probe 副本修复。
- 当前已实现部分 / 目标未实现部分：C++ selector 已验证错误名称没有 Data hit 且 fetch counters 不增加；real NFD/MiniNDN route、missing layer object、授权重绑定/revoke、protected restart 和完整 T006 仍未实现或观测。
- 兼容性、迁移或撤回影响：仅增加测试 coverage 和 evidence，不改变 wire、生产 transport、取消/deadline、cache identity 或 public API；撤回只需移除测试负例及 B190-29 evidence/log entry。
- 源码与证据：`tests/integration-tests/di-prepared-request.t.cpp`；[B190-29](../specs/190-multiturn-latency/evidence/b190-29.md)。
- 验证与状态：普通根 `spec185-prepared-request` build PASS；selector `r3/r4/r5/r6` 全部 `rc=0`；状态 `ADVANCE/PARTIAL`，T006 未完成，T007 继续锁定。
