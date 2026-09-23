# Spec190 Audit

## Protected Reuse Ordering Repair — 2026-09-22

**Finding**：当前严格串行计划将 T006 `ProtectedMaterialReuse` 放在 T007
`ResidentSession` 之前，且 `plan.md`/`tasks.md` 已要求 T006 在本任务内冻结
durable key-reference、serving recovery、new-grant binding 和 retention policy；但
`contracts/design.md` 与 `contracts/material-reuse.md` 曾把同一设计门写成 T009，且
`traceability.md` 仍保留旧任务编号。这使 T006 无法闭合，也使 T007 无法按依赖合法开始。

**Repair**：将设计冻结责任统一归 T006；T006 未闭合前继续保留 normal protected
Repo 的 assembled-cache miss 门，不能以 cache-compatible 路径替代。T007 只在 T006
`DONE` 后进入；T009 仍只负责 FINALIZE/drain，不再承担 protected material 设计门。
本次只修订 active contract/traceability/audit 文档，没有修改生产代码、缓存策略、授权
语义、任务状态或运行证据。

**Coverage matrix**：

| Lane | Result | Scope and check |
| --- | --- | --- |
| production entry/callers | `covered` | 以当前 `Provider.cpp`/`ProviderArtifactCache.cpp`/`NativeCanonicalOnnxAssembler.cpp` 静态结果核对 T006/T007 owner；无源码修改。 |
| implementation and wire | `covered` | `contracts/design.md` CD-04、`contracts/material-reuse.md` CD-09、`plan.md`、`tasks.md`、`traceability.md` 的任务顺序和安全门一致性。 |
| test/harness/oracle | `covered` | T006 `ProtectedMaterialReuse` 与 T007 `ResidentSession` 的 planned C++ selectors 仍按严格依赖，未冒称通过。 |
| build/source closure | `N/A` | 纯文档修订，无新增符号、target、安装或源码闭包。 |
| migration/evidence | `covered` | T003 保持 `PARTIAL`，T004–T011 保持 `NOT_STARTED`；当前 protected miss 与 run-58 raw evidence 不变。 |

**Validation**：修订后运行 Spec Kit sync、strict structure、serial task order 和
`git diff --check`；产品 compile/link/runtime/dynamic 均 `NOT_RUN`。Closure decision：
`CLOSED_FOR_VALIDATION`（仅本次顺序修复），下一稳定出口仍为 T003 完整闭合后启动 T004。

## Per-Change Static Review Gate Revision — 2026-09-22 14:04 -0500

**User intent**：将“每次修改完成后先做主动只读静态审查，再构建/测试/实验”写入
Spec190及其执行文档，避免在未复审的工作树上反复试错；不改变T003首个资源边界、任务顺序或
T004启动条件。

**Scope**：仅审查并修改 `spec.md`、`plan.md`、`tasks.md` 和
`checklists/requirements.md` 中的治理条款；没有生产源码、测试、launcher、配置、模型或证据
raw 被修改。当前工作树含其他既有未提交改动，本次范围按上述四个路径精确隔离。

**Static findings**：none。新增门明确规定固定baseline、完整diff、真实入口/caller、owner、
测试/oracle、Waf/安装/符号闭包、五lane coverage、四类 miss、首个失败边界、review trace 和
closure decision；同时明确 `STATIC_PASS` 不等于行为、性能或资格PASS，后续修改使受影响复审失效。
Spec、plan、tasks、checklist 对该门的术语和当前T003/T004状态一致。

**Coverage matrix**：

| Lane | Result | Scope and check |
| --- | --- | --- |
| production entry/callers | `N/A` for product change | 本次无生产代码变化；既有T003入口/边界由 `tasks.md` 与 `evidence/b190-03.md` 保持引用，未将文档审查冒充代码审查。 |
| implementation and wire | `covered` for document contract | `spec.md`、`plan.md`、`tasks.md` 的统一静态门文本及 `STATIC_PASS` 边界交叉核对；无wire/API变化。 |
| test/harness/oracle | `N/A` for product change | 未修改fixture、selector、oracle或Waf注册；结构与任务前置检查覆盖文档可执行性。 |
| build/source closure | `N/A` for product change | 未新增入口或符号；未触发C++构建，避免将文档检查写成compile/link证据。 |
| migration/evidence | `covered` | 保留run-34 raw边界、T003=`PARTIAL`、T004–T011=`NOT_STARTED`；仅新增本审查记录和当前checkpoint。 |

**Validation**：`verify-spec-kit-sync.py --require-entrypoints` PASS（11/11，personal shared skill
present）；`audit_speckit_structure.py specs/190-multiturn-latency --strict` PASS（17 FR、9 SC、
5 stories、11 tasks、17/17 traced requirements、2 tasks complete）；repository prerequisites
PASS；`git diff --check` PASS；ID交叉检查保持T001–T011、FR-001–FR-017、SC-001–SC-009和B190-01–11。

**Compile/build misses**：N/A（纯文档治理修改，未运行产品构建）。

**Runtime/test misses**：N/A（纯文档治理修改，未运行产品测试；T003既有normal Repo资源边界仍未关闭）。

**Dynamic validation**：`N/A`，无运行时状态变化；不产生`DYNAMIC_PASS`。

**Build measurement**：N/A；没有新增C++ target、source closure或安装产物。

**Behavior result**：文档范围 `STATIC_PASS`；产品范围保持T003 `PARTIAL`，不解锁T004。

**Review trace**：baseline `63ac255684c880ded77ebfd9761b0f5c49ff7b7a`；完整diff范围为上述四个文件；执行
`verify-spec-kit-sync.py`、`audit_speckit_structure.py --strict`、repository prerequisites、
`git diff --check`及ID交叉检查。使用的skill/reference SHA：
`.agents/skills/speckit-specify/SKILL.md`=`aa3a6fa946bd181300aa638a830f9e43153fb72cbe9c93168dc9a54d230d94f1`，
`.agents/skills/speckit-audit/SKILL.md`=`adcecbdad090b8000b686d11a1873b823ae43cdc7bf811b2f8d813fbfd3bc46a`，
`pre-test-static-review.md`=`2ac9b07aa65a6a8e88f2a37a146439b193401241431d756a087e679955d3e95c`，
`batch-quality-gates.md`=`08bb7431984cba703bb7a7f96c4024d09681529db4e48cdce548d440f5667159c`。
本次不调用review-agent进行产品代码审查，因为没有生产代码差异；文档审查采用上述官方skill、
结构检查和交叉检查，未把缺少agent调用写成产品静态PASS。

**Batch growth decision**：不扩批；仅把已有执行规则提升为单一Spec190治理门。

**Closure decision**：`CLOSED_FOR_VALIDATION`（仅本次文档治理修改）；下一稳定出口仍是T003内
针对run-34 `MODEL_MATERIALIZED/WORKER_START`资源边界的静态审查与最小Changed gate。T004–T011
继续保持`NOT_STARTED`。

## Strict Serial Revision — 2026-09-22 03:05 -05:00

用户要求：清单即执行顺序，上一项全部本项验收完成才进入下一项，不允许前向依赖或批内欠测推进。
以`832cc8b7`为基线重排未执行任务，映射见tasks checkpoint；旧B190/证据身份保留。
当前ID：T006传输、T007固定Repo、T008 prepare复用、T009保护材料、T010收敛、T011系统实验。
下文旧版审计中的T ID均为历史编号，不作为当前调度依据。

**Findings / repair**：修正原收敛/实验排在追加前置之前；正文/registry/plan统一单链DONE依赖。
Kant指出T010 Read包含后项、plan仍可并行及T004/T006本项/系统验收混淆，均修正并局部复审PASS。
T004本项原生控制/生命周期完整验收，T006本项真实序列化/计数校准完整验收；
真实MiniNDN≤2秒及保护材料热路径完整归对应T009/T011，原SC不减少、不把欠测改名为完成。
共享skill增加按用户显式要求启用的STRICT_SERIAL，默认模式不变；本机tasks入口和版本化模板引用同一规则，个人共享副本同步。
Laplace检查器审查发现子ID截断与模式声明宽松，改完整ID解析/显式声明并加反例；单层检查器明确拒绝可执行子ID。
Laplace对该增量复审PASS；main实际运行正常链+10反例PASS，不以代理未运行的静态意见代替测试结果。

**Coverage matrix**：production entry为本机speckit-tasks入口及版本化模板；implementation为task-progress严格模式与顺序检查器；
test为正常链和10个非法结构fixture及真实tasks解析；build/source closure为Python工作流工具、native N/A；
migration/evidence为旧→新映射、保留历史证据及局部DONE/系统PASS边界。无产品源码/API变化。
**Validation**：strict结构17 FR/9 SC/11 tasks/0完成；顺序检查及自测、skill格式、sync11/11+personal、diff检查。
**Review trace**：profile沿用下述review-agent路径/SHA；Kant审Spec顺序/验收，Laplace审skill/检查器，均只读。
初次冻结`.codex-tmp/spec190-serial-review/review-v1.tgz` SHA256 `7b78a2b7d124039dd3c0cb89a29454f795411e79f66e53d305989f685f78269b`；
后续只对上述已定位问题修复并复审。Batch growth decision：仅顺序/验收归属/skill门修订，不扩产品任务。
**Retrospective**：static发现/修复如上；compile-link N/A；workflow-runtime为检查器自测，native-runtime NOT_RUN；
unobserved为全部产品任务与真实性能。Closure decision: CLOSED_FOR_VALIDATION（文档/工作流），不表示产品完成。
**Checkpoint boundary**：仅Spec190与干净的任务模板/进度契约/新检查器；既有21个暂存文件不混提。
本机`.agents/skills/speckit-tasks/SKILL.md`受忽略，已更新但不强制入Git；个人共享副本仅本机同步。
Design变更索引仍含既有混合修改，保留未提交。下一步仅T001；T009生产编码仍有安全设计门。

## Verdict

PASS — 限定本次文档规划：新增Repo/传输/prepare复用范围完成主审、结构检查及Kant独立复审修正。
结构PASS：17 FR、9 SC、5 stories、11 tasks、0产品勾选、17/17 FR追踪。
不是全Spec READY_FOR_IMPLEMENTATION；T004先诊断FINALIZE首边界，T011先冻结CD-09安全接口。
产品实现/行为/性能NOT_RUN。以下原7任务审查保留为历史，不代表扩展范围已获独立PASS。

## Scope Revision

相对checkpoint `930f69e9`增加CD-06–09/T008–T011：实际stage tensor预算与分层缓存计数，
固定每node Repo owner和重启恢复，prepare前lookup免重复拆层/导出/打包/STORE，当前授权下合法复用。
已修正旧版“Repo全部不在范围”、7任务追踪和仅STORE去重的歧义。
源码核对表见research；已有file backend/manifest-first基础不等于真实protected重启复用。
CodeGraph有待索引文件，结论以直接源码核对为准。校验读盘不等于网络payload，layer hit不等于resident hit。
五lane按plan B190-08–11；static主审完成，compile-link/runtime-test NOT_RUN，
unobserved为实际prepare跳过量、wire预算、重启后合法密文服务及磁盘稳定性。

### Revision Review Trace

review profile沿用下述路径/摘要。两reviewer首次遇model capacity，保留状态后重试；不视为产品失败。
Kant针对冻结快照发现T010只恢复publication receipt不足以恢复PreparedModel；已补ModelPreparationCache前置分支、
PreparedMetadataV1、reference-only catalog/adapter恢复、当前Runtime owner重建及真实冷正/热零计数，局部复审PASS。
v1 `.codex-tmp/spec190-reuse-review/doc-snapshot.tgz` SHA256 `a9bdad06aa9eabdbddcf13a6042281e34889c202f24b38c5535011bfb3372bb9`；
修正后v2 `.codex-tmp/spec190-reuse-review/doc-snapshot-v2.tgz` SHA256 `44c276bc9d29f9c2449a2b94edf0ab3efff9b9bc64e54b73fe1b3dded9fdbd09`。
结构检查PASS，Spec Kit入口同步11/11，diff whitespace检查PASS；未执行产品构建或实验。
Laplace完成冻结快照的传输/缓存范围复审，无新增控制性发现；T010最后增量由Kant复审，不冒称两人均独立重验。
Closure: CLOSED_FOR_VALIDATION（规划交付），T004/T011前置门保留，所有产品任务仍未完成。

## Scope and Source

用户要求多轮token生成延迟改善、1秒ACK候选、详细任务、排除非核心内容。
源码在Experimental脏工作树，HEAD b9c6930b46e2bf9d91c5ae5732403bb5728db003；
历史r260 raw保留。真实行为/样本口径见research；文档不能冒充新运行结果。

## Findings and Repairs

| Severity | Finding | Repair / detection |
| --- | --- | --- |
| HIGH | 将30秒归成stop固定等待没有证据 | research/CD-03/T004改为FINALIZE控制闭环，先定位首边界，保留补偿语义 |
| HIGH | 旧profile若重绑定会冒充当前请求 | CD-04分离load provenance与新request observation，C++污染反例；CUDA资格不放宽 |
| MEDIUM | checkpoint issuedAt及Provider执行事件容易被称TTFT | research标清来源；T001/T007新口径成对重测，不混合历史数字 |
| MEDIUM | 只缩ACK遗漏CLI缓冲、同handle及轮间等待 | T003实时消费/持久对象，T004终态闭环，SC逐阶段独立报告 |
| MEDIUM | ACK early-close增加复杂度但收益最多约1秒 | 本Spec剔除，先固定1000ms；不改通用服务默认或新增自适应算法 |
| MEDIUM | 虚构RequestHandle.cpp路径 | 改为现有PreparedModel.hpp/.cpp，核对Conversation实际const签名 |
| MEDIUM | EventReader内存fixture不能证明CLI即时flush | T003/CD-02加真实CLI子进程pipe、暂停后续生成、暂时空队列和失败退出反例 |
| MEDIUM | FINALIZE缺部分提交反例 | T004/CD-03加一侧COMMIT/另一侧ACK丢失，不提交成功checkpoint并补偿 |
| MEDIUM | single-flight发起者取消owner不明确 | CD-04明确cache-owned load与请求waiter分离，T005加首loader取消/全部取消/close后迟到完成 |
| MEDIUM | decode主指标含义不清 | SC-006固定requester接收间隔、首token/EOS/finalize排除和逐轮配对统计，ORT耗时单列 |
| MEDIUM | 默认一槽范围及explicit evict/close准入不够明确 | 仅Spec190 CPU profile显式启用；补evict(key)及Retiring/closed拒绝新租用、drain语义与并发反例 |
| MEDIUM | ASan不能覆盖shared cache的data race | T005加限定生产owner的TSan ResidentSessionConcurrency；不扩成第三方ORT整体race资格 |

## Coverage and Evidence Limits

五lane规划覆盖见plan；未实现测试/Waf注册明确planned，任务均未勾选。
只读CodeGraph+源核对、r260日志派生分析；本轮无产品修改、编译、安装、MiniNDN或清理模型。
Dynamic profile/build/runtime均NOT_RUN（纯文档工作）。T004生产修复设计门仍需真实首边界，不能全Spec标READY_FOR_IMPLEMENTATION。

## Review Trace

main agent完成source-reality和任务双向追踪；Laplace在冻结v1关闭其四项发现和handle术语问题；
Kant在冻结v2确认启用范围/evict/close问题关闭，仅要求加限定TSan gate，已在plan/T005补齐。
v1 `.codex-tmp/spec190-planning/review-v1.tgz` SHA `2f53e6b0a53c89de49831a40bd944349b0c22b530694892a1ee4d7555ae70530`；
v2 `.codex-tmp/spec190-planning/review-v2.tgz` SHA `b9d26d3fa32bd30259ee14434a96d1cfb261c7c2378b43f765c73bf50e6032de`。
两agent均只读，无委托产品修改；Kant追加核对最后TSan两处delta并返回planning PASS，所有所提发现关闭。
profile `/home/tianxing/.codex/skills/review-agent/SKILL.md`，
SHA `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`。
无DeepSeek调用。Batch growth decision：只保留7个核心行为/验收任务，不拆行政子项。
结构gate：12 FR、6 SC、3 stories、7 tasks、0产品勾选、12/12 FR映射；10份文档相对文件链接与7行进度映射通过。
Spec Kit sync PASS 11/11；feature pointer与本机managed plan指针已切190，Context Mode索引刷新后project/active health通过。
Closure decision: CLOSED_FOR_VALIDATION，仅表示本次规划文档交付闭合；产品实现/性能验证未开始。
Checkpoint scope：仅新Spec190十份文档和`.specify/feature.json`；旧Spec189交接及Design记录含既有混合改动，
保留工作树不混提。本机AGENTS managed pointer在忽略文件内，仅本机更新。使用hook既有
`NDNSF_LOCAL_CHECKPOINT=1`文档checkpoint入口，保留禁止路径检查，不改hook、不使用no-verify、不push。

## Batch Retrospective

static：如上术语、范围和FINALIZE归因已修；compile-link：NOT_RUN；runtime-test：NOT_RUN；
unobserved：1秒真实成功率、真实TTFT/吞吐、FINALIZE首个缺失点、CPU驻留节省量及退出资源。
本轮不补造构建耗时，不用任务数量作性能证据。

## 2026-09-23 Direct INT8 source-compatibility audit

### Audit boundary and verdict

本轮针对“只替换原始模型文件，FP16→预构建 INT8 后为何连续出现多处错误”做跨层只读审查。
审查范围为 Python candidate inspection/cache、C++ model descriptor/catalog、canonical graph and
initializer identity、Repo/material publication、selection-bound assembly、ORT runner 的
input/KV/output contract、cache key、T003 evidence 与 failure index。CodeGraph 已同步，当前
source reality 以 Experimental 工作树和 r6 raw evidence 为准。

结论：当前缺口不是一个 adapter 缺失，而是 source-format compatibility 没有作为一条完整
生产契约实现。INT8 候选的公开 IO/KV 已与当前 ORT smoke 对齐，但在进入 ACK 之前，inline
initializer 被 publisher 当作单个 oversized payload，首个真实边界是
`DI_NATIVE_PUBLICATION_MATERIAL_PAYLOAD_TOO_LARGE`。因此尚不能宣称 assembly、runner、两节点
协议或多轮 KV 兼容，也不能把错误归因于“INT8 本身不被 ORT 支持”。

### Findings

| ID | Severity | Finding | Consequence |
| --- | --- | --- | --- |
| F190-INT8-01 | HIGH | `quantization` 已进入 Python recipe/profile，但 `NativeModelDescriptor` 的 canonical JSON/identity 没有独立的 quantization subtype；C++ preparer 只要求非空字符串。 | FP32 public contract 与 weight-only INT8 representation 容易被混写；跨量化 cache/recipe 复用无法由统一 descriptor 完整拒绝。 |
| F190-INT8-02 | BLOCKING | publisher 仍把 inline initializer 序列化成一个 material payload，并保留 1 MiB 上限；已有 chunk/reassembly 只覆盖 external initializer。 | r6 在本地 preparation 停止，未进入 ACK/Selection；提高上限会破坏内存/出版预算，不是修复。 |
| F190-INT8-03 | HIGH | Python ONNX identity 近期修正为 wire-level `TensorProto.dims`，但尚未完成干净 C++ build 和 direct-candidate dual-oracle regression。 | 量化 scale/zero-point scalar shape 的 Python/C++ 漂移可能在后续 publication/role binding 重新出现。 |
| F190-INT8-04 | HIGH | current candidate 的 `precision=float32` 只表达公开 activation/KV/output contract，语义尚未在 C++/文档中版本化为 weight-only INT8。 | “全 INT8”与“INT8 weight + FP32 activation”会产生错误 cache identity、错误 adapter 方向和错误验收结论。 |
| F190-INT8-05 | HIGH | inline chunking 的 backing/ownership、ModelPreparationCache retention、assembled material 和 provider fetch 的峰值尚未用内存账本闭合。 | 修复分块后仍可能重复保留约 754 MB source/initializer/material；不能仅以单 payload 通过证明内存问题解决。 |
| F190-INT8-06 | MEDIUM | 现有 Spec190 把“量化”整体写在排除项，tasks/evidence 还保留“下载未完成”的旧 checkpoint。 | 任务范围、当前事实和后续 gate 不一致，容易再次跳过静态审查直接运行。 |

### Ordered repair plan

修复只在 T003 内按以下顺序执行；任何一步失败都停在该步，不启动 T004，也不把 focused
PASS 写成产品完成：

1. `B190-03A-1 Contract`: 固化 `ONNX + weight_only_int8`、FP32 public activation/KV/output、
   INT64 control、full IO/KV/position/operator contract；将 subtype 纳入 C++ model identity、
   recipe/cache key 和负例矩阵。全图 INT8、signed INT8 wire、量化导出不纳入本次修复。
2. `B190-03A-2 Identity`: 对固定 8372-node candidate 做 Python↔C++ graph/initializer/shape
   双向 oracle，特别覆盖 198 个 scalar scale/zero-point initializer；完成 clean affected build
   后才允许继续。
3. `B190-03A-3 Material`: 将 inline raw initializer 接入现有 bounded chunk representation，
   复用 external chunk 的顺序、digest、range、reassembly 和 selected-layer filtering；补充
   oversized-inline、chunk tamper、order/length/digest mismatch、inline/external parity 的 C++ tests。
4. `B190-03A-4 Ownership`: 记录 source protobuf、initializer backing、material payload、
   assembled role model、ORT session、KV 的 owner/bytes/lifetime；证明 chunking 不产生第二份
   完整模型，失败和取消均释放本次 owned material，stable cache 不被 cleanup 删除。
5. `B190-03A-5 Native gate`: 真实 C++ canonical publisher/assembler/provider runner/ORT CPU
   target 验证 selected role、one-token、continuation 和 output/KV contract；仅在此处观察到
   真实契约不匹配时，才新增 adapter，并单独增加 adapter identity/negative regression。
6. `B190-03A-6 Chain gate`: static review → affected compile/link → installed binary/source
   closure → candidate preflight → 一次 fresh normal Repo。证据必须出现 prepare、publication、
   ACK、Selection、placement fetch、assembly、execute、terminal 或首个失败边界；不复用旧 run。
7. `B190-03A-7 Live turns`: 只有 B190-03A closure 后才回到 T003 原有 Conversation/live-token
   三轮测试；再按严格顺序考虑 T004。

### Coverage and closure

| Lane | Current result | Required closure |
| --- | --- | --- |
| production entry/callers | `PASS` for static route mapping: Python prepare → C++ catalog/publisher → Repo → provider assembler/runner | clean source/CodeGraph recheck after each production edit |
| implementation/wire | `BLOCKED` at inline publication payload bound | bounded inline chunk contract and ownership proof |
| test/harness/oracle | partial: ORT CPU one-token smoke and prior cold-assembly gate; no direct full candidate C++ chain | named C++ identity/material/runner selectors |
| build/source closure | stale after draft inline regression; interrupted build has no result | clean affected Waf build/install plus hash/nm/readelf closure |
| migration/evidence | `PARTIAL`; r6 raw retained, task checkpoint corrected by this audit | new immutable run, failure index, no overwrite/qualification inflation |

四类 miss：`static=BLOCKED`（inline representation and descriptor contract are incomplete）；
`compile-link=NOT_RUN`（draft regression build was intentionally stopped）；
`runtime-test=PARTIAL`（ORT smoke only, production chain stopped at preparation）；
`unobserved=ACK/Selection/assembly/terminal/multiturn KV/cleanup under direct INT8`。
Closure decision：`OPEN_FOR_NEXT_BATCH`。T003 remains `PARTIAL`; T004–T011 remain `NOT_STARTED`。
本审计完成的是全局定位和顺序计划，不是产品验收。

### B190-03A native candidate gate closure update (2026-09-23)

在不改动 production lineage 校验的前提下，完成了 direct candidate 的真实 C++ native
gate。既有测试夹具首次运行在 `OnnxRuntimeModelRunner::run` 因缺失
`GenerationEpochLineageV1` 停止；静态复核确认 stateful causal-position contract 要求
authenticated prefill/decode lineage，随后仅在 fixture 中补齐该契约及
`generationInputTokenCount`。受影响目标 `integration-tests` 重新构建 `128/128`，固定
754 MB Qwen INT8 candidate 的 selector 通过 `1/1`（约 89.5 秒），实际覆盖 canonical
assembler、OA02 worker、C++ ORT CPU session/warmup、prefill、FP32 logits 和使用前次
present KV 的 continuation。

这只闭合 B190-03A-5 的 direct native one-token/continuation contract；不等于 T003 完成。
正常 Repo 链仍必须在 root-owned context 中 fresh 执行，并出现 prepare/publication、ACK、
Selection、placement-bound fetch、Provider assembly/execute、terminal 或首个失败边界。

| Lane | Current result | Remaining gate |
| --- | --- | --- |
| production callers | `PASS` for static assembler → worker → runner route | root-owned normal Repo lifecycle |
| implementation/wire | `PASS` for INT8 weight identity, bounded material, FP32 public IO/KV, and lineage-bound continuation | Repo publication/selection/fetch evidence |
| test/harness/oracle | `PASS` for named C++ direct candidate selector `1/1` | ACK/Selection/two-provider/terminal oracle |
| build/source closure | `PASS` for affected `integration-tests` `128/128` | installed closure and root-owned candidate run |
| migration/evidence | `PARTIAL`; all first boundaries and fixed-candidate hash retained | fresh normal Repo raw evidence and cleanup |

Miss classes are now `static=PASS`, `compile-link=PASS`, `runtime-test=PASS` for this focused
native gate, and `unobserved=Repo/ACK/Selection/placement/two-provider/terminal/EOS/three-turn`.
Closure decision is `OPEN_FOR_NEXT_BATCH` for the remaining T003 normal-Repo chain; T003 remains
`PARTIAL`, T004–T011 remain `NOT_STARTED`.

### B190-03A normal-run wrapper audit update (2026-09-23)

对 normal-run prepare command 做静态闭环检查时发现一个真实编排缺口：原
`LocalExperiment.command_for()` 没有把 `--require-multi-token` 传递给已经支持该门的
`NDNSF_DI_Qwen06B_Native_Minindn.py`。这会使“3 轮、2 token 上限”在命令层仍可能退化为每轮
单 token，无法证明 EOS/预算语义。修复只增加 wrapper 参数和条件转发，并加入 Python 回归；
32 项回归与两个脚本 `py_compile` 通过，r9 prepare manifest 显示目标参数确实存在。

该修复关闭了 T003 的编排/准备缺口，但没有改变 root owner 前置条件，也没有产生 protocol
结果。r9 的 candidate/profile/binary/build/model/topology hash 已冻结，状态仍是
`NOT_EVALUATED`；下一步仍只能在 root-owned context 中执行一次 fresh normal Repo。

| Lane | Current result | Remaining gate |
| --- | --- | --- |
| production callers | `PASS` for LocalExperiment → native launcher option forwarding | root-owned process launch |
| implementation/wire | `PASS` for explicit multi-token requirement in recorded command | live EOS/token/terminal evidence |
| test/harness/oracle | `PASS` for 32 wrapper regressions and r9 prepare manifest | normal Repo/Provider/three-turn oracle |
| build/source closure | `PASS` for Python syntax; native candidate gate remains PASS | root-owned installed run |
| migration/evidence | `PARTIAL`; r8 was not executed and r9 is retained | fresh run evidence and cleanup |

Closure decision remains `BLOCKED_ON_OWNER_CONTEXT`; T003 `PARTIAL`, T004–T011 `NOT_STARTED`.

### B190-03A status update after identity/material gates (2026-09-23)

本次按既定 owner 顺序完成了两个最小修复：`NativeModelDescriptor` 将非默认
`weight_only_int8` 纳入 canonical identity，并保持 legacy `none` 的旧 JSON 字节兼容；
inline `raw_data` 改用 shared backing 和 bounded chunk view，复用 external initializer
的 digest、range、顺序和 reassembly 校验。对应 C++ identity/material selectors 分别为
`6/6`、`25/25`、`26/26`，受影响 targets compile-link PASS，Python launcher syntax PASS。

这只关闭了 source identity/material representation gate，不关闭 direct candidate native
gate。既有 native fixture 在 `NativeExecutionPlanJson.cpp:1413` 因 selection dataflow
缺少匹配 execution-role/request/attempt/plan digest 停止，未进入 assembler/ORT；不得为此
引入无关 fixture 重构。新的 direct r7 在 launcher root-owner preflight 以
`MININDN_REQUIRES_ROOT` 停止，当前会话没有 root/sudo，未启动 MiniNDN、Repo、Provider、
ACK/Selection、assembly、ORT 或 terminal。两项边界均已写入 [b190-03](evidence/b190-03.md)
和 `docs/failure-log.md`，并保留原始 run 目录。

| Lane | Current result | Remaining gate |
| --- | --- | --- |
| production callers | `PASS` for static source→catalog→publisher→assembler route | root-owned direct candidate run and production lifecycle evidence |
| implementation/wire | `PASS` for identity subtype and bounded inline material focused contract | native candidate assembler/runner/continuation contract |
| test/harness/oracle | `PASS` for named identity/material C++ selectors; fixture is `FAIL_FIXTURE_DATAFLOW` | named production C++ runner oracle on the candidate |
| build/source closure | affected Waf targets and Python syntax `PASS` | installed/source closure and root-owned normal Repo run |
| migration/evidence | `PARTIAL`; r6/r7 and fixture raw boundaries retained | ACK/Selection/placement/terminal/cleanup evidence |

Miss classes remain `static=PASS` for the repaired gate,
`compile-link=PASS`, `runtime-test=PARTIAL` (focused gates only; direct production path
unobserved), and `unobserved=direct-candidate assembler/ORT continuation, Repo, ACK/Selection,
terminal, and full multi-turn KV reuse`。Changed gate closure decision is
`BLOCKED_ON_OWNER_CONTEXT`; T003 remains `PARTIAL`, T004–T011 remain `NOT_STARTED`。
The next allowed action is one root-owned fresh normal Repo run with the same candidate/source
hashes, after candidate preflight and the existing static/source closure are rechecked；no
further speculative production edits are authorized by this update。

### B190-03A selected-layer scope audit (2026-09-23)

补充核对 production role-preparer 和 assembler 后确认：semantic node mapping、每 role 的
`nodeIndices`、layer range 以及 selected-node cover 校验均存在并已由 source review 覆盖；但
当前真实 Qwen C++ selector 的 projection 是完整 8372-node 单 role。它不能证明该 candidate
的 `0..14` / `14..28` 两 stage 被分别抽取、stage activation 被传递，或两个 Provider 都
执行过一次。该缺口只能由 r9 root-owned normal Repo 关闭，不能再靠 tiny fixture 或完整图
selector 代替。

因此当前 closure 保持 `BLOCKED_ON_OWNER_CONTEXT`：T003 `PARTIAL`，T004–T011
`NOT_STARTED`；不新增 adapter，不继续做与该 gate 无关的重构。
