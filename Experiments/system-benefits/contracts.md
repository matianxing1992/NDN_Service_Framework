# Code Design: Compact Protected Requests

## CD-13 public ACK ingress reader observation

Owner: ServiceUser Face and its existing PendingCall. Public OnRequestAck authenticates the original signed publication before observing an ACK that selection would otherwise discard. Only a live basic request with reference-reading support (explicit capability or User profile reference), current request epoch, current permission and exact signed Producer audience can start an observation. Per-call serial tickets deduplicate Producer/service work across calls, cap total work at64, and expire after5s; request termination destroys ownership. Selected/candidate ACK collection leases remain separate.

Keep the existing five-argument decryption callback wrapper; add private decodeRequestAck and a private finishRequestAckOnEventLoop serial argument. Reuse the hybrid decrypt and production profile store; no legacy fallback for observer. Decode and profile completion recheck ticket, call deadline/terminal state, positive status, request UserToken/ProviderToken, original signer, current policy epoch/ControllerVersion before learning. Failure releases only the observer ticket; no candidate, selection, authority or Controller hint is created. Catch malformed observer envelope construction without changing normal ACK error semantics. Nested PendingCall gains only serial/map; rebuild consumers and installed SDK together.

Acceptance: production C++ public-ingress oracle with original signed ACK and validated profile covers both pre-decrypt guards, duplicate work, malformed envelope with diagnostic disabled, absent capability, wrong token before metadata fetch, terminal and Controller-version changes during fetch. The explicit plaintext diagnostic seam qualifies ingress wiring only. Final encrypted two-Provider MiniNDN must show the compact basic transition, zero warm metadata fetch and unchanged success/security boundaries before matched paper experiments.

## CD-09 matched audience and process recovery validation

The two-Provider experiment uses its own hello.policies: only actual A/B receive /HELLO Provider permission and User receives /HELLO. Candidate identity and the copied runtime policy share that input/hash. Production audience admission remains conservative: any unknown authorized member requires full metadata. The previous broad-policy C08 wire run is retained as unknown-audience fallback evidence, never pooled with the new campaign. Four arms match business workload and access requirements, not authorization mechanisms. Warm compact basic requires real signed A/B ACK observations; no prefilled reader cache.

Recovery orchestration keeps the existing requester/User, Controller generation, NFD, group, credentials and writable provider NDNSF_CONFIG alive. After at least three actual mode1 executions with matching successful terminals, stop/relaunch the target Provider under the same prefix using a separate log/records file and incremented SVS session. Observe old-token receipt/rejection without handler execution, subsequent bootstrap and at least three successful mode1 executions under the new boot; preserve original payload/signer/deadline/late-response oracles. Recovery is separately labeled and excluded from performance statistics. No injected expected-boot hint or manual cache clearing. Source design and independent review precede this harness change; actual restart/rotation are not proven by fixtures or process READY alone.

The existing protected token-consumption failure branch emits a bounded WARN marker containing actual Provider, request ID and its constant rejection reason, without token/key bytes. It changes no admission or error response. The restart oracle uses this pre-input rejection marker, not a post-input route event that rejected calls never reach. Require both success-to-execution and execution-to-invocation coverage; timeouts may have executed before delivery failed. Requester records boot/profile from its existing validated Response; only performance-neutral observations are added, not authority hints.

Recovery alone uses a recorded 1000ms gap between settled invocations within its 45s cohort, giving the orchestrator time to replace Provider while User retains its legitimately issued next token. Performance gap stays0. Pacing does not pause/reset an active deadline or inject a token/boot. Schedule the next invocation once after the native terminal; the independent caller-deadline observation cannot start another invocation while native work remains. Require actual ready/window/gap metadata to match the recovery configuration; keep failed unpaced coverage evidence.

An optional separate --rotate-certificate recovery run uses maintained ndnsec key-gen -t r under the target node's HOME after the old Provider exits, then the same replacement path. Controller/User/group/permissions stay fixed. Require a changed authenticated profile name and boot, new bootstrap and verified warm executions; the unchanged-credential run instead requires the same profile identity. No old profile overwrite, User cache injection or changed invocation deadline. Both recovery variants reuse a separate stable recovery workspace; key rotation remains there, leaving the performance workspace's existing credentials intact. Node PIBs preserve the new key rather than pretending reconcile_keys (which pins only NSC identities) restores NDNSF defaults. Rotation proves this local fixture's key/certificate transition, not a general certificate issuance or revocation system.

### CD-10 unselected authorized reader observation

The early explicit-candidate ACK rejection must still reject selection/dispatch. Before returning, it may call the existing reader observation only with matching pending UserToken, required ProviderToken, current policy epoch/ControllerVersion, authoritative Producer permission and authenticated original SVS provenance. Reader observation also checks current version. This is capability caching only: no ACK candidate, selection, Controller hint refresh or invocation authorization is created by the rejected ACK. Expired/stale/foreign/malformed observations cannot establish all-audience compatibility. Native oracle exercises the actual early-return path and verifies no selection/ACK storage; real two-Provider capture must prove compact basic transition and zero warm profile fetch. Unknown authorized audience still falls back. No product API/layout change or new compatibility authority.

## CD-08 actor regression boundaries

ServiceUser::finishDecryptedResponseByName names its existing requestId parameter and uses an absent nonempty ID only to reject orphan large references with existing diagnostics before pending-only name parsing; it never reconstructs an execution route. Active routing/security remains unchanged. ServiceProvider's private PublishMessage/publishHybridMessage/publishHybridMessageCandidates/publishHybridEncodedMessageCandidates accept a trailing optional<ResponseNameV2> default nullopt. publishServiceResponseV2 creates it from the already-admitted invocation before scoped state erasure; async callbacks capture it by value. Encoded publication verifies own complete Producer, nonempty fields and exact regenerated RESPONSE Name, then uses the captured service/request ID. No-context ACK/event/response paths retain live registration resolution. No public API, actor layout, authorization or retired-success downgrade change. Existing closed-registration late response must complete negatively once; foreign/mismatched contexts must not publish. Legacy-offer fixture constructs a separate full legacy Response from real Producer certificate and original dynamic/AEAD fields, without profile reference, and reports migration-only evidence. Independently reviewed design; build/native precede install/runtime.

## CD-01 Producer-owned immutable security profiles

目标 Data 名称（U/P 均为实际 Producer 的完整 prefix）：

```text
U/NDNSF/SECURITY/name=encryption-profile/H(profile)
P/NDNSF/SECURITY/name=encryption-profile/H(profile)
```

`H(profile)` 是以 domain `NDNSF-SECURITY-PROFILE-1` 和 canonical profile bytes 调用 computeNamingDigest 得到的 SHA-256、32-byte generic component；名称中的 SECURITY 和 name=encryption-profile 保留语义，不用请求 ID。它不是 whole signed packet 的 implicit digest，避免签名/名称循环依赖。Producer 使用现有 makeProducerDataPrefix、makeNamingLabelComponent、makeNamingDigestComponent 的一致 component 编码。内容 TLV 包含 schema、producer identity、原证书完整 Name、带明确 digest scope 的摘要、validFrom/validUntil、envelope algorithm enum、稳定能力 enum。不同 role/内容/证书/保护配置产生不同身份，不将动态 token、公私钥秘密或当前授权结果放入 profile。

证书为原始证书引用，不重新署名证书。User 目前 advertisement 摘要是 full certificate wire，Provider key offer 摘要是 public key bytes；新 profile 必须明确这些不同 scope，不能把二者当成可互换证据或更改既有 binding 摘要含义。

Owner/callers：User PublishRequestV2 和 Provider PublishRequestAckMessageV2 创建自己的 profile。发布路径复用 IMS 和原始签名 Data 生命周期，扩展 ServiceUser::publishSignedAppData 当前仅接受 GRANT/DEPLOYMENT 的新命名 allowlist；Provider 接入相同发布/验证机制，不能复制另一套权限系统。必要 trust schema 明确允许各 Producer 自己的 SECURITY 子空间，不扩大任意签名者权限。Repo API 和原始 signed packet 不变。

首次请求/ACK 携带 profile 精确 Name；接收者首次获取原包、验证内容 hash、精确 Name、签名链、signer 对 producer prefix 的授权及实际证书绑定后缓存。缓存对象含 shared_ptr<const Data> 与解析后的不可变 profile。缓存 key 为完整 Name 加 validation/trust generation；缓存不是证书有效或权限有效的永久证明。首次 profile 与请求可并行获取，但 profile 验证完成前不解密/执行。warm 在本地按 Name 解析，禁止为同一有效引用重新获取。

安全描述发布走 best-route Data prefix；不为它创建 sync group。标准 protocol 名称、原证书名称以及 REQUEST/ACK/SELECTION/RESPONSE 不改。不得用 Repo prefix 替换 profile Producer。

## CD-02 Necessary request fields and canonical binding

### CD-01A shared native component (B205-02)

`ProducerSecurityProfile`（SecurityProfile.hpp/.cpp）包含完整producer Name、原始RSA encryption Certificate Data、supportedCapabilities uint32；canonical schema1使用0xF850 root、0xF851 version、0xF852 producer Name wire、0xF853原certificate Data wire、0xF854 flags。算法和validity及原certificate名称从certificate/schema推导，不重复传输；User full-certificate digest与Provider public-key digest通过独立具名方法派生，不改变旧binding语义。仅支持 RSA-OAEP-SHA256，明确拒绝unknown capability bits。wire总长不超过NDN packet bound；signature生产原包另有大小校验。

方法：`wireEncode() const -> Block`、`static wireDecode(const Block&) -> ProducerSecurityProfile`（非法抛invalid_argument）；`getName() const -> Name`调用CD-01摘要helper；`advertisement() const -> EncryptionCertificateAdvertisement`产生旧语义字段；`publicKeyDigest() const -> string`；`makeData(KeyChain&,SigningInfo, milliseconds) const -> shared_ptr<const Data>`产生本Producer签名新packet，不操作Repo。

`SecurityProfileStore(Face&, shared_ptr<MessageValidator>, capacity=64)`持有shared State；`resolve(Name exactName, Name expectedProducer, milliseconds timeout, Handler)`，Handler接收`shared_ptr<const Record>`或错误string，Record保存原Data和已解析profile。先检查精确命名/缓存有效期，hit同步返回不expressInterest；miss上限64，同名waiter上限64，合并一次Interest。Interest exact-name、MustBeFresh=false（不可变public metadata，由内容身份/证书有效期/当前授权判断，不靠Freshness授权）。网络Data必须经过现有MessageValidator和producer signer授权，再核对canonical profile、producer、hash及RSA证书identity/validity。

所有resolve/invalidate调用和handler在Face executor；close/destructor关闭状态、取消在途Interest并丢弃waiters。State mutex和delivery lease让从其他线程close等待正在交付的handler退出；不能在该store的handler中销毁自身owner。callbacks只捕获weak State及fetch ticket，每次miss有独立ticket，防止旧validation/timeout清除新同名fetch。`invalidate()`清空缓存并失败在途请求，不清除actor重放tombstone；`close()`不调用owner callback。Actor接线见CD-01/CD-03，B205-02仅共享组件及原生oracle，Actor publication/ref/resume仍必须实现才能完成T002。

Request 保留显式 wire schema，UserToken、本次 ControllerVersion、必要 profile Name 引用、策略/输入模式；targeted 另有 ProviderToken 与二进制密钥信封。requester/service/requestId 从已验证 REQUEST Name 解析；target Provider 从现有 targeted routing 编码解析。能由已认证名称唯一确定的字段可在紧凑 wire 省略，但解码后的语义对象必须完整，冲突即拒绝，不能按 rank/默认身份补值。

稳定 capability 与证书广告由 CD-01 profile 重建，消息内部仍提供当前 getUserEncryptionCertificate 等语义访问结果。本次动态 flags 用固定 TLV/enum，不重复长 field name。ControllerVersion 保留完整当前权威版本；legacy policyEpoch 只有在调用链证明完全等价且旧路径隔离后才可从它派生，不在编码第一阶段直接删除。

输入放在消息正文，不称为请求头；已有独立 signed Input Data 和 AEAD 验证保留。动态密钥信封、nonce 和 token 不外移为每次都要 fetch 的对象。basic 不携带指定接收者信封；targeted 输入即使 64 B 仍保持选定 Provider 隔离。

Targeted key carrier 新标识 `TargetedRequestScopedKeyEnvelopeBinaryV1`，值为 SelectionKeyEnvelope 原始 canonical binary wire；发送/接收改动在 ServiceUser::RequestServiceTargeted 对应 9651 carrier 路径、ServiceProvider 15129 解包路径及 detail/ProtectedTargetedRequest.hpp。DeploymentControlMessage value 已支持二进制，不引入第三方 codec。新标识排除 self carrier 后的 canonical binding transcript 和 legacy 不同，双方必须按所选 profile 一致重建；不能只替换 unhex 而漏掉 transcript/digest。

接收端至多一个 carrier；空/超长/错误 type/非 canonical/尾随 TLV/双 carrier 拒绝。makeProtectedTargetedBindingSelection 仅排除所选 carrier，保留所有其他 authenticated facts；profile reference 本身及 schema 进入 domain-separated transcript。compact binding 中稳定证书信息可由已验证 profile 重建；包内只发 necessary dynamic facts 与 binding digest，但最终 AAD 必须基于同一完整 RequestSecurityBinding，不能将 name/reference 的存在当成已通过绑定检查。

## CD-03 Cache, negotiation and failure ownership

### CD-01B actor publication and reference integration (B205-03)

Request/ACK accept one exact binary Name reference (TLV0xF855), mutually exclusive with a legacy inline advertisement. A local shared immutable advertisement alias resolves semantic getters; serialization always retains the reference and cannot materialize fetched metadata into the authenticated transcript. Request copy/clear and ACK copy/clear preserve/reset both reference and local alias. Duplicate, reference-plus-inline, noncanonical Name and trailing reference bytes fail closed.

User publishes its profile once into its existing Producer IMS; Provider likewise uses its NDNSF content filter/IMS without Repo transformation. Published original Data and precomputed record are retained per actor, renewed only on encryption certificate identity change. On first incoming reference, actor-owned SecurityProfileStore verifies and resolves it before existing Provider execution/token/replay admission or User ACK candidate admission. Warm resolution performs no fetch. Destructor closes deliveries before other owner state; Controller invalidation cancels metadata waits while preserving execution/replay gates.

User advertises `SecurityProfileReferenceV1=1` in authenticated Request capabilities before computing any protected transcript. Provider may use compact ACK reference only for this explicit reader capability. Stable recipient certificate Name, public-key digest and hex public key are omitted from the ACK key offer and reconstructed from validated profile, while dynamic boot/schema/endpoint facts remain. Local resolved key offer is excluded from ACK wire. Legacy/unknown User retains full ACK. Protected bootstrap response advertises `targeted.securityProfileReferenceV1=1`; a User enables its compact targeted Request reference only with this authenticated valid scoped offer. Unknown/mixed basic solicitation continues legacy advertisement because support of every receiver is not established. This compatibility path must not be mislabeled as fully compact basic Request.

Acceptance: native wire oracle proves resolution leaves Request/ACK bytes unchanged, resolved copies share immutable advertisements, mutual-exclusion/malformed reference rejection and reduced ACK wire size. Actor/static call-chain review checks actual producer serving filters, publication original-packet retention, invalidation and owner close; real actor and MiniNDN qualification remain required after integration. Profile misses use existing invocation deadline where available and never restart the User request timeout.

缓存由各 actor 的 Face executor 管理，大小有界，LRU 淘汰允许之后重新获取；in-flight map 合并 exact-name 获取，waiter 持有请求状态/取消标识，终态断开回调引用。沿用已有超时预算，metadata miss 不重置 request timeout。cache miss 不消耗 ProviderToken；引用验证/协商完成后才进入既有 token-consumption 和 execution gate。

证书轮换、Controller policy/version/status 更新、trust schema generation 改变或 actor 关闭时接入已有 invalidateControllerScopedCaches。不可变原包可留作内容缓存，但授权判定缓存必须失效；旧 profile 即使仍在缓存也不能执行被撤销调用。重放 tombstone 不随 profile 淘汰删除。

升级顺序为 reader-first：旧 wire 保留独立 canonical decode；新 profile 由经过验证的 ACK/能力交换显式确认后使用。basic discovery 未知 peers 不默认新 wire，使用 legacy discovery/bootstrap；warm basic/targeted 只有确认的接收者集合才启用 compact。对混合/未知 group 维持旧 discovery 后经已选 peer 协商进入 compact，不能牺牲可发现性。不从未认证的广告学习“安全 profile 已就绪”。

## CD-04 Independent acceptance and source closure

生产 source closure：NDNSFMessages.{hpp,cpp}、RequestConfidentiality.{hpp,cpp}、detail/ProtectedTargetedRequest.hpp、ServiceUser.{hpp,cpp}、ServiceProvider.{hpp,cpp}、ProducerDataNaming helpers 与相关 trust schema。复用现有 Waf framework target；不得增加平行实验协议冒充生产路径。

原生 C++ oracle 必须覆盖 binary embedded NUL、canonical bounds/尾随/双 carrier、双方同一语义 binding、跨 provider/request/attempt/version/ref 替换失败、同名不同内容失败、wrong producer signer、缓存命中无 Interest、expired/revoked profile fail-closed、missing reference timeout、并发 single-flight、取消/关闭回调清理，以及旧新版协商。发布 immutable profile 只一次，丢缓存后的重取验证不得复用旧授权。

MiniNDN 后续比较 basic/targeted 的冷启动、bootstrap、warm；保持当前业务负载/签名/策略/访问要求，报告 packet bytes 与额外 exchange 分开。新协议验证、重跑实验和论文修改已获授权，不能沿用旧 PASS。

## CD-07 known-audience basic profile reuse (T003/T004)

Retain the exact Producer/service identities from the accepted signed PermissionResponse as an optimization-only audience projection, and check it against the existing authoritative service permission snapshot on every encoding choice. Commit the projection only after permission replacement succeeds; a projection failure clears it and retains legacy encoding without aborting the original grant processing. Missing projection remains unknown. Never infer a Producer by splitting a combined provider/service Name: qualified service names can map multiple exact Producers to the same permission key. Retain all signed audience identities and recompute keys through makePermissionFullServiceName. Request audience restrictions do not establish that other authorized group members cannot receive a broadcast. Unknown or mixed audiences retain the legacy User advertisement and reader capability.

ServiceUser holds at most64 Face-owned capability observations, keyed by the exact Producer/service Name pair, not the potentially colliding combined permission key. Each observation records the exact Producer, service, matching policy epoch and expiry bounded by the resolved certificate validity and5s from a validated ACK. Record only after the existing ACK candidate-set/controller/UserToken/policy/ControllerVersion/ProviderToken gates, before already-selected candidates are ignored. Require actual trustSchemaValidated provenance, signer identity/key locator/wire digest, matching User permission and resolved exact Provider profile. This is public codec capability evidence, never execution permission or a token cache. Negative/legacy authenticated feedback conservatively removes the observation. Controller-scoped cache invalidation clears observations with the existing profile store.

Before ordinary PublishRequestV2 encoding, require every current UserPermission entry for this service to have a current matching observation at the current policy epoch. Empty/unknown/revoked/expired/different-service/different-epoch audience cannot enable reuse. On success call existing ensurePublishedSecurityProfile and encode the User exact reference/local resolved alias; omit the duplicate reader flag while retaining other Request capabilities. Provider ACK negotiation accepts the validated User reference as reader support. Preserve the frozen protected-targeted branch, AAD, paper names and native authorization/replay checks. A first profile miss remains cold retrieval; validated warm cache must issue zero profile Interests.

Native checks exercise the real audience predicate and encoding helper: authenticated versus unverified provenance, all-known versus unknown/new/mixed peers, exact full Producer/service membership, expiry/epoch/revocation/invalidation, legacy fallback and retention of other capabilities. Real MiniNDN must show compact basic signed Request and zero warm metadata refetch, together with protected basic/targeted success and negative/recovery gates. No two-RTT claim follows solely from shrinking a header.

## CD-06 Response recipient metadata reuse (T003/T004)

Extend the existing ResponseMessage with the same exact binary ProducerSecurityProfile reference used by ACK. Provider attachTargetedTokenBatch emits it only for successful protected unary refill, an authenticated reader and a current already-published local profile: Request carries the profile-reader capability or an already validated User profile reference. Otherwise keep legacy complete recipient metadata, including cold const compatibility callers. Publish/serve the original immutable Provider profile through existing ACK ensurePublishedSecurityProfile; preserve const refill APIs and use consistent atomic shared-pointer access for the cached Provider record. Do not introduce a new authority or Repo envelope. The reference replaces only public key, certificate name/digest and stable binary/inline/compact/profile-reader capabilities. Current scoped-service support, token pairs/count, boot/registration/controller generations and expiry remain dynamic and signed in the Response.

Response decoder/encoder reject duplicate references and mixed reference/legacy stable fields. Resolution keeps original wire unchanged: a local resolved token view merges the precomputed Record key hex/digest, certificate name and signed profile capabilities with original dynamic tokens. The existing token-offer consumer uses that view, preserving boot/registration/epoch/expiry/replay checks. Local resolution cannot authorize execution. Copy retains shared resolution; Clear/setTokens discard it.

User finishDecryptedResponseByName resolves before large-content handling, scoped AEAD/nonce consumption, refill admission or user completion. Resolve against the parsed actual Provider identity through existing actor-owned SecurityProfileStore. A warm hit issues zero Interest; miss coalesces exact-name validation with original remaining invocation deadline (no-deadline calls retain bounded5s metadata Interest). Invalid/stale/wrong-Producer profile fails closed and releases existing decrypt-in-flight ownership. A callback rechecks pending invocation, deadline and current controller/response admission through the unchanged response pipeline. Actor store close drains/suppresses callback ownership as already implemented. No replay reservation or user completion happens before resolution.

Native acceptance: response legacy/ref roundtrip and copy/clear, unchanged wire after resolution, duplicate/mixed references, missing resolution, contradictory recipient metadata; actual warm store path proves zero refetch with validated Provider record, cold/wrong signer/expiry cases retain existing store rejection. Negative/refill execution, token replay, controller/registration recovery and real MiniNDN protected success are required before T003/T004 completion. This batch does not complete known-audience basic Request reuse.

## CD-05 RTT closure (T003/T004/T005)

### CD-05B scoped encapsulated validation readiness

B205-04 captures show one SVS publication retrieval per message although signed piggy Data is present. MessageValidator posts to Face and to its signature worker before SVS can cache that packet. Add a narrow encapsulated-validator adapter sharing the existing MessageValidator. Only a packet of at most4096 encoded bytes already executing on the owning Face may complete its existing cached-local-certificate verification inline. Use the same signature verification, namespace and invocation-signer admission, including rejection callbacks; no unverified packet enters a cache. General MessageValidator callbacks remain asynchronous. Off-thread, large and cold/network-certificate paths retain existing dispatch and ownership. No ndn-svs source, wire naming, suppression or retry change. Native oracles cover inline cached success, bad signature, generic async ordering, large/off-thread fallback and callback Face ownership. MiniNDN must independently check whether publication-fetch traffic falls, retaining errors/loss/recovery and actual RTT evidence.

### CD-05A concrete integrated wire (B205-04)

SelectionKeyEnvelope keeps root0xF830; explicitly negotiated compact schema emits ordered schema0xF856=1, binding SHA-256 raw32 bytes0xF857, wrapped keys, created/expires. Algorithm derives from schema1 RSA-OAEP-SHA256. Legacy reader remains unchanged and rejects compact fields. New unwrap reconstructs full expected RequestSecurityBinding from authenticated invocation/validated profiles and compares its canonical-AAD digest before existing RSA/key-lifetime checks. No full binding copy is transmitted in compact mode.

Selection compact-mode field0xF858=1 is retained when normalizing out the key envelope, binding the mode in input AEAD. Optional inline input container0xF85A has ordered schema0xF85B=1 and optional original signed Data (max2048B). Digest normalization clears only Data and retains the negotiated container/mode. Absent Data uses the original exact-name/segmented fetch fallback; malformed/oversized/changed Data is rejected by the same name/signature/AEAD checks. User freezes both modes before generating keys/AAD. Provider advertises normal input-inline and compact-envelope capabilities only in authenticated scoped ACK; target uses separate authenticated bootstrap compact carrier capability and a distinct Request transcript domain.

NDNSF framework defaults maxPiggyDataSize4096/maxApplicationParametersSize6144, within the library packet bound; existing explicit environment overrides remain. No library or suppression change. Native oracles must cover compact binding mutation/recipient/expiry/reordered or trailing fields, mode-bound normalization and signed inline original wire, while real MiniNDN measures packet packing and warm 2/1RTT. Typical small single-provider workload is the RTT target; large/external inputs and cold misses remain separately reported.

warm basic 的理想序列为 Request→ACK→Selection→Response 四次单程传递（约2 RTT），warm targeted 为 Request→Response（约1 RTT）。basic 当前 Selection 后发出 scoped Input Interest，多出1 RTT；因此 T003 还需由已认证 ACK 显式协商 small signed Input Data 在 Selection 正文的 optional inline 交付，并保留原外部Data获取fallback。

新增 Selection inline字段必须与 key envelope 一起从 circular selection digest 中排除，但其协商模式、输入精确名称与完整 execution binding 必须进入认证上下文；原 Input Data producer signature/AEAD/name/replay验证不删除。大输入不保证2 RTT。现有REQUEST/ACK/SELECTION/RESPONSE命名不变。

仅缩小Request不足：common.cpp 的NDNSF SVS profile还需使典型原始signed message符合piggy预算且完整同步ApplicationParameters包含mapping/data后仍有容量。沿用库Options及环境覆盖，框架提供有界默认值；不改ndn-svs库命名或随意延长suppression。实际packet/分片/额外fetch与不同delay下RTT斜率必须通过真实MiniNDN验证。丢包恢复和cold不承诺恰好1/2 RTT，须单列。



## Phase B — CD-14–20 (planned, no implementation qualification)

新阶段引用Phase A原安全/命名/缓存契约，不重复或取消它们。每个CD的细化在其工作单元内通过源码冻结；不能把本计划表当作精确API已经实现。

### CD-14 Application descriptor and private input (T006/T009)

Owner：Core传递/保护通用opaque descriptor，应用解读类别/资源；User拥有原始business input与每调用keys，Provider只在真实选择和授权后取得输入。
existing：RequestMessage payload/capabilities、request-scoped input/key envelope、onAck/onRequest生产接口；当前DI envelope字符串清洗仅支持特定输入，不是通用descriptor契约。
planned：先核对所有公开caller与现有descriptor承载。如果满足隔离/回调可见性则复用；否则最小独立descriptor字段/API，与business payload明确区分，生产wire兼容与C++/Python调用同批协调。新旧函数签名、TLV边界、大小上限、旧peer拒绝/回退须在T006内写明后方可编码；当前DRAFT。
不以使opaque business input重新进入发现Request来修复。默认未知/emptydescriptor有明确行为，不应默认为任意任务适配；两个任务的eligibility由应用callback决定。缓存/Repo取回同样验签/命名/权限/调用绑定。保留原DI envelope执行与signedInput外部/inline行为。
native oracle必须证实允许/拒绝descriptor、缺失/畸形/超限、wrongproducer/signer/permission、nonselectedinput、nonce/proof重放、取消/关闭、并发owner与既有DI兼容。Provider实际callback输入与独立expectedeligibility比较；实验脚本不能代替Core admission。

**T006 concrete design**：复用现有受保护Request的Payload wire，无新增TLV或部署状态机。RequestMessage新增local-only可选discovery payload（owned Buffer，最大4096B），`setDiscoveryPayload(const ndn::Buffer&)`/`hasDiscoveryPayload()`/`getDiscoveryPayload()`；copy/Clear维护此字段，WireEncode不编码此本地准备参数。ServiceUser::prepareRequestScopedRequest原签名不变：先保存原getPayload到private plaintext，再优先将显式discovery payload放入Request Payload，否则保留原DI清洗/空payload行为。未使用request-scoped保护时拒绝显式discovery参数，不静默混合语义；旧peer读取原Payload，执行仍由Selection取得原业务输入，无新wire兼容路径。应用Provider ACK callback读取getPayload作descriptor，执行callback读取getPayload作原始输入。
Python request_service/request_service_select追加可选`discovery_payload=None`，转同一native setter；既有位置参数保持。原生RequestScopedSelection新增显式descriptor与private input隔离正例；原有negative/DI回归保留，setter/copy/Clear/上限由原生oracle核对。精准测试target为`spec200-targeted-boot`，selector `RequestScopedSelection`；仍需真实MiniNDN descriptor callback资格。设计READY_FOR_IMPLEMENTATION不代表行为通过。

### CD-15 Targeted material and terminal recovery (T007/T010)

**Current closure (2026-10-14 07:40 -05:00)**：T007原生/动态终态负例及当前候选loss、restart、证书轮换验证已完成，见evidence/phase-b-execution.md。原设计中的DRAFT/待验文字保留为历史；不将T007完成解释为物理丢失根因已证明、丢包零失败或T010机制归因已完成。

使用现有RequestServiceTargeted→一次性pair消费/传输→Provider判定→Response/补充材料链。分别记录prepared、bootstrap及重新准备状态，不由fast/slow分类猜测。
诊断包含Request出版、目标接收、签名/权限/epoch/key/profile通过或拒绝、executionacceptance、handler、结果取回及APIterminal。协议正常的已消费token不能复用；同request合法重传仅原不可变packet。
首个失败按source/build/runtime/harness/resource分类。只修确证缺陷；特定loss下协议不保证成功时报告失败与全attempt耗时，不将保持未知当源码根因。正常因果可说明但不得承诺loss成功率优势。native wrongtarget、replay、restart、epoch/registration变化、timeout/latecallback、cancel/close为受影响边界。
当前准确修复签名/状态字段保持DRAFT，T007先在同任务核查并冻结，不能为了baseline更快削弱保护。
**T007 terminal oracle design**：既有warm/关闭注册用例提取到runProtectedTargetedTerminalCase(bool expireBeforeResponse)，保持前三轮bootstrap/补充pair/replay检查。第四轮原5s调用handler后延迟completion；timeout分支保持注册，最多五次既有bounded Face pump等待API timeout，再两次success completion并完整pump。生产target spec200-targeted-boot / RequestScopedSelection的两个具名selector ProtectedTargetedWarmCallsReplenishAndFenceClosedRegistration、ProtectedTargetedTimeoutIgnoresLateDuplicateSuccess断言四次handler/四次terminal/pending0。该oracle证明超时后调用completion不能增加API终态；未直接观察迟到signed Response publication，不能扩展声称两份真实网络Response均被User拒绝。fixture-only，无生产wire/ABI改变；原输入不可用负例及原loss诊断仍独立待关闭。
**T007 unavailable input oracle design**：在既有FaultProfile增加可选dropDataPredicate(const ndn::Data&)；仅RequestActive时bridge在转发前按predicate丢弃原Data并计入droppedPackets，默认空保持所有原用例行为。测试仅匹配User的REQUEST-INPUT命名组件，保留Request/ACK/Selection/Response及证书/参数流。具名C++用例ProtectedTargetedMissingInputFailsBeforeExecution在原5sAPIdeadline下首次bootstrap请求，等待既有生产1s输入Interest timeout；必须得到真实失败Response、error包含input Data fetch timeout、handler0、一次APIterminal/pending0及非零实际丢包。scope reset清除fault闭包，再正常调用成功，证明不是fixture未就绪。无生产代码、签名或重试行为更改；独立正常与无抑制ASan/UBSan各一次，240s上限，target仍spec200-targeted-boot。

### CD-16 Production permission update and revocation (T008)

**Current closure (2026-10-14 07:31 -05:00)**：T008已完成；下文的待验证/待冻结措辞为设计演进记录。最终源码、原生/动态测试、原P1/P16十组资格及适用边界见evidence/phase-b-execution.md的T008 completed与相邻原始审计记录。该完成不替代T012管理成本研究或全文投稿验收。

两角色owner oracle补充：从已认证hint边界进入真实maybeRefreshControllerVersionHint，
检查更高coalesced hint换current查询、同/低hint不重派发、签名旧status不能满足highest。
异步authority测试采用MessageValidator的callbackFace生产路径，先提交有效签名Data，
在Face处理验证前更改Controller绑定，要求不安装且owner释放；恢复绑定后相同签名/status
通过异步正例，排除仅因validator配置错误而负例通过。每个验证等待用真实steady clock限3s。

补充原生验证设计：ControllerRevocationFlow/PolicyStatusRetryChangesNetworkNonceAndPreservesOriginalReply
保留500ms丢首回复/同名重试，并对User/Provider增加11000ms Interest lifetime场景，
让真实10s prepared清理先于Face timeout；第二次查询必须换名、参数不变且成功安装版本。
每角色观察上限14s，两次attempt，完整selector仍受240s外部预算约束。
这仅调整原生fixture参数，不改变MiniNDN grant10s/revoke20s、生产默认值或正式负载。
PolicyStatusOriginalReplyCapacityExpiresWithoutRenamingOldReplies通过真实Controller handler
生成4096份signed回复，核对拒绝新查询、保留原包、到期回收及旧名称不可重新发布。
两个selector新增范围均待独立静态审查与实际运行，旧nonce-only通过不覆盖新用例。

**Repair binding (combined STATIC_PASS; build/runtime validation pending)**：
Actor增量编码绑定：新增共享`detail/policy-status-fetch.hpp`中的
PolicyStatusFetch仅保存transport字段(ticket/attempts/controller/query/renewAt/pending/retry)。
controller绑定dispatcher的明确目标；回调核对ticket和完整query Name后从owner取Controller，
不依赖10s清理的prepared缓存。配置Controller改变时拒绝旧绑定并在超时释放匹配owner。
两端原service set改为service→shared_ptr<PolicyStatusFetch>，每端单调ticket计数器；
关闭/析构清map并取消handle/event，回调仅捕获weak owner及ticket，不形成引用环。
原fetchPolicyStatusFromController签名保留，委托新增dispatchPolicyStatusFetch
(同四参数+bool replace)；新hint替换与续期消费原owner剩余attempt，重复普通调用合并。
onPolicyStatusData/Timeout增加ticket参数，isCurrentPolicyStatusFetch核对service、ticket、Name；
finishPolicyStatusFetch只释放匹配owner并显式结束coordinator transport。
validData必须在异步验证前后及安装前保持当前owner，且版本不低于highestCandidate。
验证失败沿同一个owned timeout/retry入口，不能清owner后留无主retry。
dispatcher使用steady renewAt强制新querynonce；prepare辅助的短期清理不能延长活动query。
更高已认证hint(含FETCH_COALESCED)使用current查询并replace；相同/低hint不消耗新attempt。
状态安装调用preservePendingFetch=是否存在活动owner。既有direct oracle改为启动真实fetch
并使用其ticket；不增加ticket=0绕过路径。容量、迟到回调及预算native oracle仍须实现；
这些具体改动完成后固定快照复审，不能依据本设计自动放行构建。
Coordinator增量接口冻结：`installCurrentStatus(status, nowMs, signatureValid,
bool preservePendingFetch=false)`保留旧三参数调用语义；true时仅更新有效已认证状态，
不清inFlight/attempt/retry时间。新增`void finishTransportFetch()`仅清该三项，
保留未满足highestCandidate；`startFetch`不能用较低hint覆盖保留的较高candidate。
Actor接线已完成，实际运行待验证。复用controller-revocation-state.t.cpp新增
TransportOwnedFetchSurvivesUnrelatedStatusInstall原生oracle，并将该已有源加入
spec201-control-authorization；覆盖保留、终止、低hint重启、高目标完成及无效签名不改变状态。
本增量先实现共同寿命规则及Controller容量观测，不单独部署或运行资格：
utils.hpp/.cpp新增`uint64_t controlQueryLifetimeMs(const std::string& operation)`，
POLICY-STATUS返回10000、其他操作300000；`controlNonceExpiry`从实际名称operation选取
该规则。User/Provider prepared query复用helper安排清理，status重试遇身份过期或已清理
生成新querynonce，保留调用传入attempt；其他RPC缺owner仍失败。owner隔离配套完成前
整批保持PARTIAL。现有Spec201ControlAuthorization原生target新增独立时钟边界oracle，
测试真实带参数Interest的9999/10000ms、299999/300000ms与+30000/+30001ms边界；
RAII恢复ndn-cxx测试时钟，禁止墙钟sleep近似边界。容量/晚回调oracle随owner子范围完成。
`controlNonceExpiry`保留名称格式和+30s时钟容忍，仅POLICY-STATUS身份寿命改为10s；
其余RPC仍300s。Controller原包保留至身份到期，到期旧Name拒绝；4096项/64MiB上限保留，
容量拒绝须可观测。超前时间戳可导致40s保留，因此只对原同步MiniNDN负载验收10/20s恢复，
不承诺任意速率或所有允许时钟偏差下的恢复时限；native覆盖满容量、到期恢复和旧Name拒绝。

User/Provider每service的transport owner拟拥有当前Interest、派发ticket、实际尝试次数、
单调时间更新点、pending Interest handle和retry event；替换/终止时取消资源。
`PolicyRefreshCoordinator::highestCandidate`仍是唯一版本目标，不在Actor复制另一份。
网络、异步验证和重试回调改变状态前核对当前ticket和Name；身份更新、更高hint替换
不重置`permissionFetchMaxAttempts()`预算。bootstrap无status允许current查询。
协调刷新显式查询current状态并要求结果达到最新已认证highestCandidate，避免旧exact-version
循环；其他显式exact-version调用保持原契约。FETCH_COALESCED也检查是否替换旧派发，
无关status安装不能结束活动transport。拟为coordinator安装接口增加默认关闭的
preservePendingFetch选项，并显式结束transport fetch而保留未满足目标；不混用默认3次
coordinator预算与Actor预算。编码前补齐精确头文件/调用点及bootstrap、迟到回调、高hint、
无关安装、耗尽和query更新oracle；本设计摘要不构成STATIC_PASS或验证放行。

**T008 nonce retry concrete design**：沿用已冻结first Changed gate，ServiceUser与ServiceProvider的prepareControllerControlQuery签名及stored owner不变；attempt>1时先检查现有owner，复制其冻结Interest并refreshNonce后返回，不能修改存储的Name/参数/digest/querynonce或原Controller回复。所有现有RPC caller继续使用同一helper，PolicyManifest额外刷新Nonce保持兼容。具名ControllerRevocationFlow/PolicyStatusRetryChangesNetworkNonceAndPreservesOriginalReply用实际User/Provider public fetch、Face timeout及真实Controller原回复验证：只丢首次回复，第二Interest完整Name/参数相同、网络Nonce不同；两回复wire完全相同、实际签名有效并最终安装同版本。fixture可用既有500ms最小fetch lifetime/0backoff/2attempts配置加速，但原MiniNDN10/20s恢复预算及生产默认值不变。Controller cache容量修复仍另一个待冻结边界；nonce通过不能关闭T008。

生产路径由Controller签发permission/status并由Provider/User安装和使用；comparison credential transport没有执行授权权力。记录四个明确事件：controllerCommit、providerVersionInstalled、executionAccepted、responseTerminal。使用同host单调时钟，跨host若使用须明确clock误差。
撤销定义：Provider安装该服务撤销version后，新获准的被撤销调用必须为零；Controller提交至安装的传播延迟单独报告。每个受影响Provider必须在原revoke20s观察预算内安装新version，不能以始终未安装使拒绝判据空真。grant10s/revoke20s均从对应controllerCommit起算；安装、survivor恢复、访问负例分别记录。处理安装前已接受的in-flight请求由既有执行语义决定，不通过事后回包时间认定绕过。
按原8身份、P1/P16、grant10s/revoke20s、poll500ms/status24h/grace0配置复验。一个grant失败会让随后revoke NOT_RUN，不能算通过。七个survivor须真实执行成功；retainedoldkeys无法读取新generation，oldpairs/stalestatus在当前生效边界拒绝。
源码审查replycache容量/retention/唯一reply身份/轮换通知与callbackownership；缓存pressure是待检验关联，不先写为根因。具体行为变更、退出路径及负例在T008冻结；正常密码/身份流程不可由脚本绕过。

**T008 first Changed gate**：原c04 P16抓包已见Controller发状态Data而Provider未收到，后续同查询名重试无出网；User/Provider的prepareControllerControlQuery在attempt>1返回wireEncode过的原Interest，未刷新网络Nonce。先对返回副本refreshNonce，完整应用Name、ApplicationParameters、业务querynonce及原回复字节保持。PolicyManifest已有独立refreshNonce不作为PolicyStatus已正确的证据。具体Nack类型未知，不冒称抓包已证明Duplicate Nack；native须丢首回复并断言第二次Interest同Name/参数而Nonce不同。
另五个P16原Controller恰好4096份原RPC，4042–4064份PolicyStatus，四次revoke晚于最后publication且新查询被容量gate拒绝；这是独立确认的恢复边界。不得直接LRU后重签同旧Name或无界扩容。短status query身份寿命/新查询代次及旧回调owner需先冻结配套合同再实现；该第二边界仍OPEN，本次nonce修复不能单独关闭T008。

### CD-17 Coordination experiment and statistical contract (T009–T011)

**T009 concrete application contract**：沿用现有dynamic actors与oracle，不改Core。
descriptor是canonical JSON四字段：version=1、task（1..1e9）、class（compute/memory）、
phase（measure/warmup）。ACK读取生产Request Payload，execution读取恢复后的64B私有输入。
私有输入采用`T205|M或W|C或M|十位十进制task|private-input`并以`_`补齐64B；
descriptor不包含private-input标记。此固定模拟输入不代表真实UAV计算。
两Provider的模拟compute/memory容量分别为(8,4)/(4,8)，两任务需求为(4,2)/(2,4)。
可用且两资源均足够才eligible；按min(1000*capacity/demand)降序、完整Producer名升序选一。
两类在正常状态均有两个候选，但偏好相反；同规则用于ACK与signed advertisement，
不存在为某臂预先指定正确Provider。ACK回显descriptor及资源/观察时间，User验证任务绑定；
execution重新核对当时资源，状态变化可合法失败，不把选择当资源预留。
warmup也用合法schema/两Provider均适配的任务，通过同一解析和授权路径。
首个代码增量仅迁移上述任务/资源契约及既有native oracle；不运行正式实验，
完整T009还须预定到达/有界owner迁移、native日志oracle、runner/analyzer及资格。
共享到达序列在T009完成，T010仅增prepared/recover-required对照，不形成循环依赖。

**T009 arrival/owner increment**：runner在每paired cell启动前生成一个只读task-plan.json，
所有臂共用相同字节/hash；含version/window_ms/period_ms和按序task、arrival_ms、class。
相对arrival从0起，低负载4700ms、高offered-load候选100ms，最多600个任务；正式负载矩阵
仍待pilot冻结，旧period=0闭环不作为新实验模式。User独立定时派发，逻辑deadline固定为
原arrival+2s，max inflight=1/queue=0；过载记rejected，已过deadline记timeout，均保留分母。
最后arrival后在t1停止到达并观察至t1+3s（2s最大deadline+1s回调drain），不由terminal驱动next。
Face拥有active task和arrival/drain事件；每task拥有一次refresh round（至多两pending Interest、
一个timer），终态/替换/退出取消，round回调只捕获weak owner，不可复活已终止task。
Core调用继续原600ms且受剩余deadline约束，迟到响应只记录；worker析构/drain保持原作用域。
本增量完成前不运行MiniNDN；native计划格式/边界oracle与独立完整记录oracle随后接同一入口。

**T009 native archive check**：扩展现有dynamic-oracle的`--manifest PATH --run-id ID`模式，
读取同目录冻结计划及已停止的User/两Provider JSONL；拒绝重复/遗漏arrival或terminal、
错task/class/private command、错误attempt/request/Producer绑定、无执行的成功和同request重复执行。
独立按明示资源真值表/trace计算eligibility与候选最优者，不调用actor的taskScore或信任eligible标志。
ACK选择须匹配真实Provider ack_handler状态与任务；广告选择须保留选择时全部cache快照以核验最优者。
所有成功须有匹配的非迟到native response和已完成的正确执行；warmup单列，不纳入cohort。
此离线原生检查证明记录一致性和应用语义，不能代替T006对象加密/授权负例。
runner在每臂停稳后调用已安装oracle，记录返回值、输入/输出hash和binary身份；只有全部臂通过
才置record_oracle.qualified。分析器仍独立复算统计，旧v1结果只沿旧语义读取，不混池。
同入口增加有界合成记录的故障注入自检，至少覆盖漏任务、重复终态、错选择/执行绑定和假成功；
合成检查不是MiniNDN资格。最终正式矩阵由有界pilot后冻结，旧30cell全交叉campaign不默认启动。

**T009 campaign migration**：既有dynamic_campaign.py必须显式接收`--matrix`，不自动
生成旧period0矩阵。JSON含version=1、purpose=pilot/formal、design_reference和cells；
design_reference指向相对矩阵目录的既有设计/冻结记录，保存其hash。每cell精确含
period_ms(100/4700)、dwell_ms(0/500/5000)、seed(非负int)、window_ms(3000/60000)，
formal只允许60000；1–128个非重复cell按文件顺序运行，四臂顺序仍由seed平衡。
保存矩阵原字节/hash、设计记录hash及候选身份；语法合格不代表研究设计获准，正式
冻结仍须满足上述pilot/统计/贡献核对。此入口当前仅覆盖E1及旧周期trace资格；
E2恢复设置、E3 irregular trace在所属任务扩展，不能用此格式宣称已完成。
campaign每cell由一个systemd transient service拥有整个MiniNDN进程树，KillMode=control-group、
TimeoutStopSec=45s、RuntimeMaxSec=600s后备上限（覆盖500s监督等待及清理余量）；不改变业务deadline。超时/中断先向main发送SIGINT
让runner保存证据并收尾，45s后仍未退出则停止整个unit；不能只kill Python而遗留actor。
每cell退出后stop同一unit清除残余，服务/收尾失败保持PARTIAL，不据此重跑或覆盖原证据。

**Treatment arms**：solicitation及刷新广告+productiontargeted。广告100ms、1s、change+1s三个已有策略先资格检查；所有Provider提供同等signedcapability/resource信息，User可以匹配任务、刷新、重选。最终比较全部合格策略并声明成本/完成率最优各自含义，不只保留弱基线。
**Shared controls**：生产权限/签名/信任/每调用输入结果保护；同topology/linkdelay/loss、任务内容/trace/到达时间、逻辑2s期限与每task最多3submit/单submit600ms及剩余budget。沿用现有60scohort和warm/readiness；prepared/recovery设置额外precondition在pilot冻结，不偷换deadline。
对齐的是访问要求与对象/执行检查，不是要求两臂使用相同消息、token或bootstrap机制；这些机制差异及其成本正是比较对象。Provider能力差异属于任务适配条件，不能因此削弱任一臂授权。
**Arrivals**：预先生成taskID/类别/arrival/deadline；deadline从预定arrival起算，排队、client延迟、容量拒绝均计入cohort。使用相同有界inflight/queue规则，先冻结上限，不用无限backlog。Provider只看自己的当前状态，不能向User公开未来trace。closed-loop作为独立throughput资料保留，不代替同arrival主对照。
cohort只含预定arrival位于[t0,t0+60s)的任务，分母在运行前固定。停止新到达后至少观察至最后一个任务的逻辑deadline，并保留预先冻结的有界callback drain；deadline后的成功记late而非按时完成，未提交/队列拒绝也必须有terminal分类。主逻辑延迟从预定arrival至terminal或deadline截止；native attempt延迟从实际API提交至terminal，未提交任务没有attempt延迟，不能赋零或删除。
各臂使用相同抓包起止与drain规则；分别报告60s到达窗口、drain及setup/maintenance流量，cohort成本含窗口和drain中对应调用/恢复流量。arrival在窗口内、按原deadline完成的跨窗任务仍计为该cohort成功。共享背景流量不能按成功任务数分摊后隐藏；每完成任务成本的分子及时间边界明确，零完成为NA。抓包覆盖drain；未排空或capture不完整保留为无效测量/未解决终态，不填零、不删除、更不能qualified。
**E1 suitability**：至少2task类别/2异构Provider，先stable+prepared低负载核对task-awarefeedback与等信息广告；真实protecteddescriptor进入callback，生产C++独立oracle验证选择/执行/解密。
oracle区分选择依据的快照时间、Provider执行接纳时的当前eligibility及terminal；状态在选择后变化可导致合法拒绝/失败，不假设选择产生资源预留，也不将其误判为选择器源码错误。
**E2 attribution**：在pairedblock开始预设prepared/recover-required，保持恢复策略安全等价并记录生产事件。若不同机制天然不同恢复，报告总策略效应与交互，不声称已经隔离纯feedback；其反事实设计尚需T010冻结，不能仅按观测bootstrap事后筛样本。

**T010 E2 implementation design (2026-10-14; formal matrix not frozen)**:
Compare `prepared` with `reregister` after identical successful warmup of both
Providers. Use the existing public ServiceRegistration.close()/generation()
and ServiceProvider.addScopedService() APIs on the Provider Face thread.
Keep Provider process, identity/certificates, Controller permissions, status
publisher session and monotonically increasing advertisement sequence intact.
This measures service re-registration recovery, not a device crash or isolated
feedback effect. No Core cache clearing, private token injection or security bypass.

In dynamic-provider.cpp, extract the existing two handler objects into local
owned values reused by a small registration lambda. One harness SIGUSR1 after
warmup applies the configured condition: prepared retains its registration;
reregister closes and creates one new generation with the same handlers.
Record before/after generation, condition, timestamp and an acknowledged marker.
Reject repeat preparation signals; SIGINT/SIGTERM retain existing shutdown.
Rearm async_wait after SIGUSR1; cancellation is benign but other signal errors,
duplicate preparation, exceptions or incomplete required preparation return
nonzero. Each registered request handler captures its own atomic generation
holder by value, published once from the returned public handle; never read
the mutable current-registration handle from an execution worker.
The signal is experiment lifecycle control only and never admits a remote task.
Keep signal callbacks inside the Provider lifetime; do not create worker threads.

run_dynamic.py passes the condition to both Providers, retains their log paths,
waits for User warm-ready, signals both and waits for both preparation markers.
Only then publish the shared origin ten seconds ahead, retaining the current
settling interval. No business invocation is inserted after re-registration.
Both conditions use identical task-plan bytes, arrivals and2s/3attempt budgets.
Add the condition to the manifest and native receipt context. Missing or failed
preconditions invalidate the run and use existing owned-process cleanup.
E2 uses `ndnsf-dynamic205-e2-v1`; absent E2 selection retains existing E1v2
format and lifecycle. New required E2 fields cannot be silently applied to E1.

Provider execution records include actual RequestMode and current registration
generation. User warmup and attempt-response records include authenticated
targeted providerBootEpoch/registrationGeneration/expiresAtMs when present;
absence is represented explicitly, not inferred as warm or bootstrap. Native
archive validation binds precondition to warmup and origin, checks unchanged
generation for prepared or distinct nonzero generation for reregister, and
checks every measured execution uses the resulting generation. Bootstrap
evidence must come from actual production request-mode events rather than the
outer targeted API call. Preserve failed and late attempts in the full cohort.
Both successful warmup responses must precede each preparation marker. Bind
their boot/generation/expiry to the old registration; require prepared materials
to remain valid at each Provider's first scheduled task, with the production
expiry margin. Recovery must strictly increase registration generation. Record
production send/reject events by request ID where available: execution-only
mode cannot explain a request rejected before entering the handler. If that
boundary is unobservable, report the overall re-registration effect and mark
specific stale-token/rebootstrap causality unverified.
Predefine the first two planned tasks as the recovery-sensitive cohort (one
preferred visit per Provider), alongside all60s tasks. Never select that subset
after observing bootstrap, success or failure. Freeze its paired estimands and
multiplicity treatment with the formal matrix after the bounded pilot.

Extend existing dynamic oracle fixtures with missing/duplicate/out-of-order
preconditions, unchanged recovery generation, wrong execution generation and
misbound response generation; production token rejection remains covered by
the T007 native generation fence and must be rechecked if its path changes.
Qualify actors/oracle/runner first, then run a bounded paired wiring pilot with
both conditions and all four arms before choosing the formal matrix. Keep
historic E1 archives readable; E2 schema/receipt additions must not reinterpret
older evidence. E2 pilot size, resource budget and formal statistics require
review before launch; this implementation design alone authorizes no formal run.

**E3 boundary**：复用E2actor，stable及慢/快irregular变化、低/持续offeredload。先单因素pilot确认变化时间/RTT和负载主轴，再冻结少量正式cells；不立刻全交叉。周期0.5/5s旧trace仅历史参照。

T011 trace implementation binding (2026-10-14; pilot/formal NOT_FROZEN):
extend existing LocalState with an optional owned immutable vector of
`AvailabilityChange {offsetMs, available}`. Before origin both Providers remain
available for warmup; at origin use the entry at offset0; at a transition use
the new value (half-open intervals); after the final entry hold its state through
drain/shutdown. No timers, threads or mutable shared trace state are added.
`decodeAvailabilityTrace(Json, producer, windowMs)` accepts exactly version1,
producer,window_ms,tail="hold",changes; each change has unsigned offset_ms and
boolean available. Require known exact Producer, matching window in1..60000ms,
1..256entries, first offset0, strictly increasing offsets below window, and
alternating states. Reject ambiguous/extra fields, negative/floating/overflow
offsets, invalid identity, missing initial state or unsupported tail semantics.
Raw parsing also rejects duplicate JSON keys and nesting deeper than8; actor
and independent archive reader enforce this before decoded-field validation.
LocalState constructor takes this vector as an optional final argument; reject
mixing it with nonzero dwellMs or forceUnavailable. Store by value before
registering callbacks; all queries are const except atomic origin publication.
Provider CLI --availability-trace loads at most64KiB before Face creation and
validates identity/window; User receives no trace path or future observations.

The existing runner gains --availability-profile legacy/stable/slow/fast;
legacy is the unchanged E1/E2 path. E3 requires prepared recovery, dwell0,
normal5ms handler and no forced-unavailable. Save two Producer-owned trace
documents before actor launch and bind their bytes/hash in manifest and native
receipt. stable means both always available. slow/fast use complementary states
with equal per-Provider50% occupancy over the window, initial Provider determined
by seed; six versus sixty balanced cycles at60s. Per-cycle durations vary using
a deterministic seed-based shuffle of bounded integer weights; apportion half
the window exactly across cycles, using the same duration for the two states
within each cycle. Thus speed changes do not also change duty cycle; no total
outage or independently failing Provider claim. Final state explicitly holds.
No trace-based reselection or early advertisements: existing ADS_EVENT detects
changes in its5ms tick; all four arms share the same frozen task/trace bytes.

Extend dynamic-oracle's existing archive path with an independent JSON trace
validator and linear replay (not LocalState or actor lookup). Check state at
snapshot/ACK observation and execution admission times, and bind both trace
hashes in receipt. Preserve E1/E2 parsing. Component fixtures cover transition
minus1ns/exact transition, pre-origin/tail, malformed times/identity/fields and
mixed legacy modes; archive fixtures reject changed trace/hash/state and keep
all planned tasks including rejected/timeout in the cohort. Runner/analyzer
must recognize E3 explicitly, never reinterpret it as E2 or a legacy archive.
Existing fixed arrival scheduler,2s logical deadline,3attempts,inflight1/queue0
remain unchanged; low/sustained load uses4700/100ms periods. High-load results
measure the whole strategy under this admission bound, not Provider capacity.
Qualify this wiring before a bounded single-factor pilot; formal matrix, sample
count, estimands and precision remain a subsequent frozen part of T011.
Pilot orchestration reuses dynamic_campaign matrix version3, adding
availability_profile to each prepared/dwell0/60s cell. Label and identity include
profile; source/design/candidate freezes and stop/cleanup use the maintained
supervisor. Version3 formal accepts only the fixed design below after its
aggregation validation. No second launch or state-management pipeline.
T011 E3 formal design:16independent paired seed blocks20590–20605, fiveconditions
ordered initially stable4700,slow4700,fast4700,stable100,fast100. Block index i
rotates this list left by i%5, then reverses when (i//5)%2 is1. Allfiveconditions
use the same seed; existing fourarm rotation remains unchanged and balanced
across16blocks.80cells/320arms/79296planned tasks,60s/prepared/dwell0/5mshandler,
original2s deadline/3attempts/inflight1queue0. Pilot is excluded. No optional
replacement or extra sampling.48000s supervision budget retains maintained
500s cell wait/600s unit backstop/45scleanup; not a strict total wall-clock cap.
Freeze actualmatrix before launch and stop first process/qualification failure.

Thirty primary paired contrasts: each of fiveconditions, SOLICIT minus each of
threeADS arms, for allplanned failure-penalized mean andwindow+actualdrain IPbytes
per success. Equalweight seedblock differences, n16, t15 critical3.8219733981616257
(scipy.stats.t.ppf(1-0.05/60,15)), approximate simultaneous95% Bonferroni family
separate fromE1/E2. Any missing value makes that entire contrast mean/CI NA;
retain its16positions, never delete a zero-success block.100ms and20KiB CI
halfwidth targets describe precision, not passing thresholds or power claims.
100ms and10KiB are reporting effect thresholds, not UAVrequirements and not
convertible to one netbenefit score. Preserve totalbytes, bytes/plannedtask,
successfraction, failureclasses andconditional timing as descriptive metrics.
Do not infer interactions from differing significance or claim Providercapacity
from single-inflight workload. Stable has bothavailable; slow/fast each have
50%complementary availability. No slow100 or complete speed×loadinteraction.

Formal support reuses dynamic_campaign.load_matrix and summarize_dynamic.py:
one fixed e3_formal_cells helper defines complete orderedmatrix and is checked
by both; no actor/runner API change. New interval parameter selects E3critical
while retaining E1/E2 defaults. Independently encoded fixtures check exactorder,
wrongseed/missing/duplicate/condition rejection, arithmetic/NA andE1/E2 regression.
Formal launch requires both source review axes plus offline/actualpilot checks.
E3 pilot aggregation extends summarize_dynamic.py with explicit schema dispatch
and the existing candidate/design/matrix/native-receipt checks. Require all five
profile/period conditions per seed, matching task-plan bytes for a seed/period
and both Producer trace hashes for a seed/profile across loads,
prepared/dwell0/60s and exact13/600planned tasks. Group by profile/period/arm;
preserve all failures and missing per-success costs, expose raw paired differences
and descriptive means only. No E1/E2 interval constant applies to E3 pilot.

T010 formal orchestration/statistics increment: reuse load_matrix/campaign and
summarize_v2, with matrix version2 carrying recovery_condition per cell and
campaign schema ndnsf-dynamic-campaign205-e2-v1. Existing E1/version1 and legacy
archives retain their interpretation. Formal E2 has16pairs/seeds20560–20575,
two consecutive conditioncells per seed. Pair i uses prepared-first when
(i+i//4)%2==0, reregister-first otherwise; this jointly balances each of four
arm rotations with both condition orders. Labels append condition; propagate
the condition into runner and requalify all32cells against frozen candidate,
matrix,design,raw receipts and full13task denominators before aggregation.

Six primary contrasts are (SOLICIT_reregister−SOLICIT_prepared) minus the same
change for each of three advertisement arms, for firsttwo planned-task
failure-penalized mean and fullwindow+drain IPbytes/completion. Complete pair is
the statistical unit; preserve all16differences, any missing component makes
the whole corresponding mean/interval NA. Paired Student t15, sixBonferroni
two-sided simultaneous95%,critical3.0362832225931182. Precision targets100ms
(5%of2sdeadline) and20KiB; fixedn16 is not a pilot-derived power guarantee.
Missed precision targets are reported without extra sampling. Fullcohort and
condition-specific latency/terminal/setup-window-drain costs stay descriptive.
E1/E2 intervals belong to separate predefined families, not joint coverage.
Budget21600s for32cells with existing500s supervisor wait/600sunit backstop and
45scleanup; stop first invalid/no replacement. Formal launch requires the
increment's independent review and frozen matrix/design artifact. A single
pilot pair establishes observability, not reliable variance or significance.
**Statistics**：trial为独立seed/trace pairedblock，所有arms使用同一预生成任务/状态trace；记录attempt、logicalterminal、bootstrap、eligiblechoice、授权执行、success、late、timeout、error、rejected。成功条件mean/p50/p95与全attempt耗时/按时completion、全流量/每completion分开。CI的statistic/paired单位/算法预先声明。零成功时条件列NA。
E1稳定条件的seed只控制类别起点及arm顺序，不生成独立随机负载；统计单位应称独立运行的配对块。
全任务主延迟代价`all_task_failure_penalized_mean_ms`：按时成功取实际终态减预定arrival，
其他终态（包括快速rejected/attempts_exhausted）一律取完整logicaldeadline减arrival。
这是失败惩罚指标，不冒称失败任务实际等待了2s；原terminal/deadline-bounded耗时保留为描述统计。
原生oracle继续负责终态正确性；离线统计测试覆盖快速失败不能优于同任务成功、成功恰在deadline不得计为成功。
任何主配对成本缺失时，完整样本主CI为NA并报告缺失块，不能删块后继续使用完整样本自由度。
T009汇总迁移：`summarize_dynamic.py`为v2单独核对matrix.json及design.md快照hash、
逐cell原生回执/四臂/13任务分母、全部runtime库与四binary身份、包含oracle的源码及contracts身份。
历史v1仍走旧路径，v2当前只接受stable/4700ms/60s E1；不宣称支持未冻结的E2/E3。
pilot仅描述；正式E1候选方案固定16配对运行seed20521–20536，六个主差值为SOLICIT减三广告臂，
指标是failure-penalized全任务代价和window+drain字节/完成任务。若正式冻结采用该方案，
使用df15、双侧Bonferroni同时95%区间，critical=3.0362832225931182，stdlib计算均值/样本标准差。
这是假设配对运行差值近似独立且适合t近似的区间，不是分布无关保证；所有原始差值保留。
缺失成本不删除，完整主CI不可估；实际半宽未达目标不触发补跑或判无效。
正式矩阵及阈值依据仍须在执行记录冻结后才能启动，代码允许该格式不代表实验已经获准放行。
pilot独立保存并排除formal。freeze正式重复数、实用最小effect与CI精度目标、seed集合和multiplecomparison处理；不依显著性补跑/停跑。n=5不自动成为新trace充分样本。nativeoracles验证taskidentity/authorization/execution，离线Python可重算统计，不替代native行为。
pilot启动前列明候选cells与总运行/时间上限；达到上限仍不能支持正式设计时保留INCONCLUSIVE并报告设计缺口，不无限试跑。正式矩阵仅保留能回答E1–E3的预定对照，扩展须明确新问题且另冻结，不事后追逐有利区间。
原始IP/data/signature/capture边界按既有analyzer检查，配置读取/统计版本绑定candidate。少数独立主表与一张必要边界图足够，具体图数不是验收条件。

### CD-18 Continued permission-management comparison (T012)

沿用nativecredential-cost/network/receiver/archiveoracle与原三方案；distributioncomparison与productionControllerrecovery分开。
初始P1/4/16及8身份为起点，保留sharedaccessmatrix，再加入一份同等用户需求的sparsematrix。更新分为多权限少更新、连续grant、revoke/rotation；各scheme用相同principal/service权限和事件顺序，均允许有效cache/credentialreuse/batching。recipient仅自己的privatekey。
顺序必须包含实际keygeneration→原signedData分发验证→访问probe→下一update；不能重复单次导出的快照假装policy增长。实际permission集合与generation变更由C++independentexpectedaccessmatrix判断。实现现有CLI的sequence/generation支持和runner/analyzer，避免新授权authority。
累计CPU分别测量实际sequence的issuer生成/签发与recipient验证/解密/更新；每段操作只计一次。component benchmark与包含同一操作的network actor CPU是不同测量层，不能相加成累计成本；若无法覆盖完整CPU则报告已覆盖部分，不标为总成本。IP按相同linkwindow定义；setup/管理/businessprobes分开，恢复失败保留窗口/计数。报告总成本、逐操作成本和访问正确性；交点只在实测sequence穿越时报告，没有穿越则给观察范围。
端到端累计CPU若报告，来自同一实际生成→分发→安装流水线内互不重叠的issuer/recipient进程CPU区间之和，不用墙钟等待代替CPU。访问probe与额外oracle处理另列；各方案包含相同必要管理操作，不把生产验证时间剔除为测试开销。
sequence长度与操作数、sparseoverlap、CPU/流量primarymetrics及正式repetitions在pilot冻结；不能因某臂失败调小权限或延长预算。construct-specific撤销语义/安全性质如不等价须说明，不能通过删除对方优化制造聚合收益。

### CD-19 Candidate, qualification and frozen history (all tasks)

Phase B每次正式campaign保存一个候选身份：HEAD+dirty源码manifest、Waf构建/安装target及DSO、有效env/topology/runtime、任务/更新trace、oracle/analyzer身份；旧HEAD本身不代表dirtycandidate。
Spec203/204/205Aformal目录和结果只读；允许按原配置在新候选/新目录复验，禁止覆盖原记录或将新结果冒作旧候选结果。source变更后的旧nativePASS不会自动继承。先具名selector/生产入口资格再performance，scope失败只暂停相关实验。执行前确定single-active主体及锁；复用scripts/ndnsf-di-workspace.py或已有等价稳定lockedworkspace，模型/安装大副本不放/dev/shm，durableevidence不在reset scratch。
T007/T008无无条件的前序任务依赖；若T006/T007改变各自实际调用路径，则对受影响候选重验。共享文件编辑、安装与正式campaign的互斥属于调度约束，不转写成无关任务的语义依赖。
T009独立设计/native oracle/pilot不要求T008先完成；T012comparison transport同样独立。正式运行仅对其实际调用的共享路径要求最新资格，后续修复影响该路径则重验；先行候选与最终候选的版本对应必须明确。T010涉及恢复行为，依赖T007；最终T013仍要求T008工程恢复闭合。正式campaign使用中的系统安装库/程序不得被另一批覆盖。
native新fixtures必须注册root/externalWaf既有sourceclosure；未来具体目标/符号map和dynamicprofile按shared门禁记录。当前无新导出/编译产物，不伪造nm/installedcheck。
formalnegative/failedcells不可覆盖，修复后的campaign新identity、新目录并保留原cell。旧论文baseline与原Data字节保持。

### CD-20 Claim alignment and artifact publication (T013)

最终出口为spec.md的P1–P6基本投稿标准全部通过，不仅是生成两份PDF。T013审查整篇canonicalpaper与支撑证据，按两个独立review轴确认创新论证、两项研究问题答案、正确性、公平统计、文字图表和复现材料；在现有完成记录逐项记录证据位置、结论与未解决问题，不新增审查文档任务。证据不足以回答核心问题或支持系统贡献、或重大问题未关闭时T013/Phase B不能DONE，即使T009–T012已科学完成；不要求每项机制都有正收益。
验收记录复用evidence/phase-b-execution.md，每行包含P编号、最终论文位置、源码/配置及原始结果引用、实际检查者与方法、PASS/PARTIAL/FAIL、未解决问题及所属任务。重大问题定义与最终放行规则唯一维护在spec.md；稿件或证据改变时只复审受影响条目，不能沿用不匹配候选的PASS。提交级清洁PDF及比较PDF须来自同一最终稿；比较基线保持只读。
同一记录附精简的主张对应表，按spec.md“主张验收单位”逐项核对，直接引用已有实验记录而不复制数据。先对主张和证据做核对，再据此修正文稿；不得先写优势结论再寻找支持样本。独立检查者从原始结果重算主表中的数值、分母及失败计数，并核对关键归因是否由对照支持；只复述作者结论不算独立审查。未独立重复的真实实验与已独立重算的统计明确区分。
缺口处理沿用原任务：实现/正确性问题回T006–T008，机制与收益证据回T009–T012，表述/引文/图表问题在T013修正。每个缺口说明可观察关闭条件；补验先冻结有界问题与矩阵，不为达到正结果无限加样本。证据充分但收益有限时收敛主张、解释适用条件；证据仍不足时保持PARTIAL。这里不新增任务或单独报告。
Related Work及关键机制来源需核验原始文献，新增引文须真实支持关联陈述。凭证复用不是ABE独有；区分复用与聚合的贡献及实测CPU/流量范围。最终对旧论文实验同样核对可解释性和版本映射，不要求无关全量重跑；影响核心结论的缺口须有界补验或明确阻塞。不得通过删除两项研究问题或减弱验收规则获得投稿PASS。
在canonicalpaper把三项贡献分别指向E1/E2/E3、targetedqualifiedhistory、新permissionsequence及productionrecovery。旧benchmark若无可核验版本/安全配置，只作有界早期结果；不由新Figure默认保护图推定旧数据覆盖新路径。
初稿/最终文字应用review的publication-relevance原则，保留negative/CI/phase/cost层次，排除debug流水、内部qualification叙事及未经实测支持的交点推断；保留“观测范围内未出现交点”的必要结论。Abstract150–200词，名称用直观user/provider/service/requestid示意，recipient编码在正文说明，不引入enc(P)图缩写。不恢复admission-control内容。
三方案credentialreuse都可用；跨应用trustschema管理收益没有实测就不宣称。选择目标venue后再核对具体格式/政策，不在本Spec假设会议已接受。
T013必要文字修订使用已安装academic-humanizer作语言检查，再按review复核内容必要性与证据；保留数字、公式、引文键和协议术语，不以润色代替P1–P6验收，也不另建润色报告任务。
复用build-paper.sh生成clean/comparison，NDNSF_Base只读。迭代时检查改页；最终放行时通览clean PDF全部页面，并核对comparison的全部改动，涵盖图表、参考文献及跨页布局；记录两份最终输出身份，不能仅用早期改页检查代替最终稿检查。最小复现包含必要maintainedcode、输入trace、原始小结果、独立nativeoracle及统计命令，不复制bulk每轮run或新造管理报告。Git边界沿用AGENTS；本Spec写作不授权commit/push。
