# Execution Unit Contract

**Profile**: model-independent execution units
**Status**: design inventory; current progress: [Execution Progress](../tasks.md#execution-progress)

## Authority and Dispatch

本文件将 T001--T017 展开为可分派行为卡，不改 FR/SC/CD/PO、父任务依赖或正式验收。
接口与算法仍以各详细契约为准；[work units](work-units.md)定义父任务验收，
[tasks](../tasks.md)是进度唯一入口。卡的存在不表示 READY。
按[共享执行规则](../../../skills/speckit-code-design/references/bounded-executor.md)执行。

T001 的设计卡由设计者收口；执行者 执行已冻结的实现卡。Reviewer 指明需要更强模型复核的边界，
不是自动调用模型或派生 agent 的授权。T015 的整体语义审查由具备跨层审查能力的执行者负责。
T016 可由 执行者 按冻结命令操作，但不能自行改变 oracle、隔离边界或解释未分类失败为 PASS。

每次分派必须记录 `git rev-parse HEAD`、相关未提交 diff、卡 ID、契约/fixture 身份及实际 build 目录。
父任务 prerequisites 与卡 Depends 取并集；同文件卡串行。T001 未完整关闭时，只执行设计卡，
不因局部 O 项关闭提前开始产品迁移。设计已有有效关闭证据时复用，不能重新运行已通过探针。
当前 O 状态查最新 tasks 与对应契约；本文件不复制容易过期的 OPEN/CLOSED 快照。

执行者先读 AGENTS、指针、tasks 当前 checkpoint、plan 的 Gate Order，以及父任务契约；
随后按卡 Read 顺序读取精确章节/符号和其调用方。源码用 CodeGraph 定位后核对。
本轮不默认读整个 contracts 目录，也不要求从全仓库搜索猜出任务范围。
发现遗漏的公开类型、生命周期或算法决定，记录具体冲突并回到 T001-C；普通局部实现选择自主完成。

## Path and Contract Map

以下前缀按 repo root 唯一展开；`N/Foo.hpp` 是精确路径，不是搜索模式。
Write 中新文件均为 planned；已有文件只改 Steps 指定符号。Read 的 planned 源只能在前置卡交付后读取。
所有实现卡可同时修改 `tests/wscript` 中自己的注册项；其他 build 修改必须在卡内列出。
每卡可更新 `specs/182-native-di-python-bindings/tasks.md` 的短结果和一份该卡 evidence；
失败才按仓库规则追加 `docs/failure-log.md`，这不是产品源码范围扩张。

| Prefix / reference | Exact path / section |
| --- | --- |
| N | NDNSF-DistributedInference/cpp/ndnsf-di |
| A | NDNSF-DistributedInference/cpp/adapters |
| P | NDNSF-DistributedInference/ndnsf_distributed_inference |
| U | tests/unit-tests |
| I | tests/integration-tests |
| F | tests/fixtures/spec182 |
| S | specs/182-native-di-python-bindings |
| CD-001--010 | [code design](code-design.md)，各同名 CD 章节 |
| CD-011 / CD-012 | [spec Code Design Index](../spec.md#code-design-index)，分别转 proof 与 plan Delivery |
| CD-013 / CD-014 | [runtime boundaries](runtime-boundaries.md)，同名 Preparation / Provider Host 章节 |
| Symbols / Values | [symbol design](symbol-design.md) / [value contracts](value-contracts.md)，只读父任务 CD 对应类型和方法行 |
| Proof | [proof design](proof-design.md#planned-test-and-build-inventory)，层级、路径与独立 oracle |
| Assembly | [ONNX design](native-onnx-assembly-design.md)，OA01--OA09、S1--S8 |
| Normalization | S/contracts/initializer-normalization.md；T001-A负责随设计checkpoint交付，未提交草稿不能作为实现分派基线 |
| Tokenizer | [dependency design](native-dependency-design.md#frozen-production-integration) |
| Stream | [token stream design](native-token-stream-design.md)，API、算法、caller、requester acceptance |
| Sampling | [generation design](native-generation-design.md#sampling-source-changes) |
| Host | [provider lifecycle](native-provider-lifecycle-design.md)及 CD-014 |
| Isolation | [isolation design](native-isolation-design.md) |

## Verification Commands

以下命令模板的所有变量必须在分派记录中解析为实际值；不允许空 selector、全文件混合层级或无测试成功。
`UNIT_BIN` 是当前依赖闭包构建的 Boost unit runner；`BUILD_DIR` 来自当前 Waf cache，不能默认复用旧 build。
`CPP(selector)` 表示 `timeout --kill-after=5s 120s "$UNIT_BIN" --run_test='<selector>' --report_level=detailed`。
卡中 `Spec182.../*` 是 **planned suite selector**，不是声称当前存在。
T001-C 冻结名称和路径，所属实现卡在 tests/wscript/对应测试文件注册，并用
`"$UNIT_BIN" --list_content` 核对非空测试集合后才运行；冻结前不能把猜测命令交给 执行者 执行。
需要编译时沿用 AGENTS 原生 preflight 和已核对 cache，`./waf build -j4 --out="$BUILD_DIR" --targets=unit-tests`；
编译上限按当前 build 计划登记，120s 仅为 focused unit 运行上限，不套用到整库重建。

`PY(file, expression)` 表示 `timeout --kill-after=5s 120s python3 -m pytest '<file>' -k '<expression>'`；
expression 必须由所属卡注册并由 T001-C 确认只包含本地 unit，不以过滤得到零测试作为 PASS。
`DOC` 表示依次运行：

```bash
.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks
python3 specs/182-native-di-python-bindings/checklists/validate_design.py
git diff --check
```

`L0`：按 CD-009 必要构建/安装，在新 staging prefix 用 pkg-config 的实际 include/link 信息编译
`tests/standalone/spec182-installed-consumer.cpp`，核对真实库路径/符号/依赖；具体 prefix、命令与限额由 T001-C 冻结。
`FINAL`：仅 T016 使用 Proof 及 Isolation 冻结的 manifest/case-id/new-run-dir，完整 unit→integration→MiniNDN/no-Python。
这些模板不能替代缺失的环境或 selector 决策。所有 I/真实进程隔离/网络 case 随实现编写，T016 才运行；
worker crash/cancel 等真实子进程案例也转 T016，卡内只运行纯 framing/状态/算法 unit 及必要安装链接。

## Execution Cards

### T001-A Identity and Dependency Closure

- **Parent**: T001; **Depends**: none; **Reviewer**: design owner
- **Read**: Assembly → Normalization → Tokenizer → S/contracts/native-dependencies.json；P/adapters/onnx/graph.py::canonical_onnx_identity；F/dependency-probes 的现有 reference evidence。
- **Write**: S/contracts/native-onnx-assembly-design.md; S/contracts/initializer-normalization.md; S/contracts/native-dependency-design.md; S/contracts/native-dependencies.json。
- **Steps**: 核对稳定 numeric 与版本化表示的独立向量、descriptor 兼容处置和依赖身份；复用已关闭项。仅补实际缺口，不复刻旧错误或升级 oracle 以抹平差异。
- **Verify**: DOC；按现有 dependency contract 核对持久 probe 结果，缺动态事实时保留未关闭项，不执行产品验收。

### T001-B Lifecycle and Capability Closure

- **Parent**: T001; **Depends**: none; **Reviewer**: design owner
- **Read**: CD-001/CD-007/CD-013/CD-014 → Host → Stream → Symbols/Values；ndn-service-framework/ServiceProvider.hpp::addService/addCollaborationHandler；P/app_sdk/placement.py::AutomaticStreamingHandle。
- **Write**: S/contracts/runtime-boundaries.md; S/contracts/native-provider-lifecycle-design.md; S/contracts/native-token-stream-design.md; S/contracts/symbol-design.md; S/contracts/value-contracts.md; S/contracts/compatibility-manifest.json。
- **Steps**: 收口 registration generation、late ACK/Selection、共享 lease 与 stop；完成公开类型/调用方、journal 兼容和原生端口清单。接口缺口写入唯一详细契约，禁止让实现卡临时补定义。
- **Verify**: DOC；逐 source symbol→contract→caller→task/PO 双向核对；没有注销 API 不可编造 unregister 调用。

### T001-C Dispatch and Selector Freeze

- **Parent**: T001; **Depends**: T001-A, T001-B; **Reviewer**: design owner
- **Read**: Proof → 本文件全部卡 → S/contracts/work-units.md；tests/wscript::build；S/contracts/code-design.md#open-questions。
- **Write**: S/contracts/spark-execution.md; S/contracts/proof-design.md; S/contracts/code-design.md; S/contracts/work-units.md; S/contracts/compatibility-manifest.json; F/case-manifest.json; S/plan.md; S/audit.md; S/traceability.md。
- **Steps**: 固定实际构建环境、L0 命令与每卡 planned suite/case；核对新增源和跨卡调用方无遗漏、共享文件串行、所有旧能力有回归。尚缺叶子设计的卡补完后才 release；不靠空测试或结构 PASS 关闭 T001。
- **Verify**: DOC；从实际 Waf 注册推导命令，所有 planned 测试有 author owner；父 T001 完整验收满足后才允许 T002。

### T002-A Installed Library Boundary

- **Parent**: T002; **Depends**: T001-C; **Reviewer**: ABI/build review
- **Read**: CD-009 → CD-001 → Tokenizer；wscript、examples/wscript、tests/wscript 的 DI source/use；N/NativeProviderRuntime.hpp。
- **Write**: wscript; examples/wscript; tests/wscript; NDNSF-DistributedInference/ndnsf-distributed-inference.pc.in; N/NativeInferenceClient.hpp; tests/standalone/spec182-installed-consumer.cpp。
- **Steps**: 提取 existing DI runtime 为同一安装库、公共头与 pc；声明 planned dependency inputs，但未实现的 bridge/worker 不加入不可构建目标；consumer 调已有符号，不伪造 requester 成功。
- **Verify**: L0；独立 consumer 无复制 DI 源、无 Python link；后续 T006/T007 将各自新源接入同库。

### T003-A Qwen Split Candidates

- **Parent**: T003; **Depends**: T002-A; **Reviewer**: local source review
- **Read**: CD-002 → Symbols/Values；P/adapters/qwen/placement.py::QwenThreeStageSplitter；N/NativePlanning.hpp（由本卡新增共享声明）。
- **Write**: N/NativePlanning.hpp; A/qwen/NativeQwenPlanner.hpp; A/qwen/NativeQwenPlanner.cpp; U/di-native-planning.t.cpp; wscript。
- **Steps**: 按冻结类型声明 strategy/registry 端口，实现 Qwen cover 和确定性候选；保持支持范围和预算，不将模型名判断放入 Core。只声明其他 adapter 端口，不返回伪结果。
- **Verify**: CPP(Spec182QwenSplit/*)；固定小图合法 cover、边界 budget、非法 rank/图输入；expected 来自冻结旧 splitter。

### T003-B Yolo Split Candidates

- **Parent**: T003; **Depends**: T003-A; **Reviewer**: local source review
- **Read**: CD-002 → Symbols/Values；P/adapters/yolo/adapter.py::Yolo26Splitter；A/qwen/NativeQwenPlanner.hpp 的公共协议。
- **Write**: A/yolo/NativeYoloPlanner.hpp; A/yolo/NativeYoloPlanner.cpp; U/di-native-planning.t.cpp; wscript。
- **Steps**: 实现现有 YOLO component cover 与候选排序，复用共享不可变类型；图输入检查与候选约束都在 native adapter。
- **Verify**: CPP(Spec182YoloSplit/*)；固定 cover、错误 component/rank、确定性重复调用；不加载服务协作。

### T003-C Placement and Registry

- **Parent**: T003; **Depends**: T003-A, T003-B; **Reviewer**: local source review
- **Read**: CD-002 → Values；P/planner/presplit_first.py::propose_v3；N/NativePlanning.hpp。
- **Write**: N/NativePlanning.hpp; N/NativePlanning.cpp; U/di-native-planning.t.cpp; wscript。
- **Steps**: 实现 registry 与默认 placement，固定同一 snapshot 时间，先兼容过滤再 residency 排序；不做 I/O、不授权、不把 has_model 当 exact residency。
- **Verify**: CPP(Spec182NativePlanning/*)；lease/budget/device/ref tie-break、非法向量、同输入同结果；与冻结 Python proposal 对照。2026-09-07 修正旧不存在的 Spec182Placement selector；当前局部用例不覆盖完整 device/residency proof，T003-C 保持 PARTIAL。

### T004-A Canonical Plan Sealing

- **Parent**: T004; **Depends**: T003-C; **Reviewer**: wire/security review
- **Read**: CD-003 → Values；P/sdk/placement.py::PlanSealerV3；N/NativeExecutionPlanJson.cpp 的 projection parser/validator。
- **Write**: N/NativePlanSealer.hpp; N/NativePlanSealer.cpp; N/NativeExecutionPlanJson.hpp; N/NativeExecutionPlanJson.cpp; N/NativeRequestPreparation.hpp; N/NativeRequestPreparation.cpp; U/di-native-plan-sealer.t.cpp; U/di-native-planning.t.cpp; wscript。
- **Steps**: 实现 sealCore/grantView/finalizeSecurity/project/encode 这一条规范封印能力；共享字段定义，保留 Provider 独立校验和所有 scope/endpoint/ACK 绑定。
- **Verify**: CPP(Spec182PlanSealer/*)；固定 wire/签名字节精确一致，错误 endpoint、缺 grant、错 ACK digest 拒绝；真实 Core commit 留 T016。 辅助 CPP(Spec182CanonicalJson/*) 只验 typed canonical 字节，不替代完整计划验收。

### T005-A InProcess Authority

- **Parent**: T005; **Depends**: T004-A; **Reviewer**: security review
- **Read**: CD-004 → Symbols/Values；P/security/artifact_policy_authority.py::ArtifactPolicyAuthority.issue；N/NativeGrantVerifier.cpp。
- **Write**: N/NativeArtifactPolicyAuthority.hpp; N/NativeArtifactPolicyAuthority.cpp; U/di-native-grant.t.cpp; wscript。
- **Steps**: 按冻结 request/policy 构造 issue，调用既有签名/recipient encryption 原语；密钥 handle 注入，拒绝 caller 自授权限。
- **Verify**: CPP(Spec182GrantAuthority/*)；固定证书/时钟向量、wrong-recipient/key/expiry 原因码，secret 生命周期；不引入网络 authority。

### T005-B Requester Grant Publication

- **Parent**: T005; **Depends**: T005-A; **Reviewer**: security/lifetime review
- **Read**: CD-004 → runtime-boundaries Cancellation and Observer Contract；P/security/requester_grant_pipeline.py；ndn-service-framework/ServiceUser.hpp::publishSignedAppData。
- **Write**: N/NativeGrantClient.hpp; N/NativeGrantClient.cpp; U/di-native-grant.t.cpp; I/di-native-requester-grant.t.cpp; wscript。
- **Steps**: 实现 acquire 的签名请求、进程内 issue、Face publication port、grant binding；工作 executor 等待且可取消，晚到回调不恢复成功。编写真实 Provider 消费 case。
- **Verify**: CPP(Spec182GrantClient/*)；publication port 单测与 timeout/cancel/reject；真实 publication/fetch/Provider 用例由 T016 运行。

### T006-A Canonical Source Identity

- **Parent**: T006; **Depends**: T002-A; **Reviewer**: normalization/compatibility review
- **Read**: Assembly S1--S4/OA05/OA06 → Normalization；P/adapters/onnx/graph.py::canonical_onnx_identity；F/dependency-probes 的冻结 identity vectors。
- **Write**: A/onnx/NativeOnnxRecipeAssembler.hpp; A/onnx/NativeOnnxRecipeAssembler.cpp; U/di-native-onnx-recipe.t.cpp; wscript。
- **Steps**: 实现 owned source、内存 external 校验、原图 identity 与版本化 normalization；复用 ONNX/protobuf，不从 external location 打开文件，不把 shape-inferred 图算作原图。
- **Verify**: CPP(Spec182OnnxIdentity/*)；稳定 numeric/v2 独立摘要、overflow/非法路径/错 descriptor 拒绝。私有算法经同 TU test seam 验证，不把私有 ONNX 类型公开安装。

### T006-B Certified Extraction and Wire

- **Parent**: T006; **Depends**: T006-A; **Reviewer**: byte-contract review
- **Read**: Assembly S5--S7/OA04/OA07/OA08 → Proof；F/dependency-probes/onnx-probe.cpp 的官方库调用；旧 Spec181 assembly 向量。
- **Write**: A/onnx/NativeOnnxRecipeAssembler.cpp; U/di-native-onnx-recipe.t.cpp。
- **Steps**: 在副本 shape inference、按边界抽图及 local function、逐 node 精确 cover、确定性编码和 ORT CPU load；不复制完整源 metadata 代替 Extractor 规则。
- **Verify**: CPP(Spec182OnnxExtraction/*)；固定 model bytes、inline/external/functions/缺 I/O/非法 cover；expected 不由新 assembler 生成。

### T006-C Bounded Native Worker

- **Parent**: T006; **Depends**: T006-B; **Reviewer**: process/lifetime review
- **Read**: Assembly OA01--OA04、Native Worker Framing and Lifetime；N/NativeCanonicalOnnxAssembler.cpp::runPythonHelper 的旧 cancel 边界。
- **Write**: A/onnx/NativeOnnxRecipeAssembler.cpp; A/onnx/NativeOnnxAssemblyWorker.hpp; A/onnx/NativeOnnxAssemblyWorker.cpp; examples/DI_NativeOnnxAssemblyWorker.cpp; examples/wscript; wscript; U/di-native-onnx-recipe.t.cpp; I/di-native-onnx-recipe.t.cpp。
- **Steps**: 接上固定匿名 pipe 协议、限额/partial frame 状态、deadline/poll/kill/waitpid；main 只调库，无路径命令入口。注册原生 worker 并编写 crash/cancel 真子进程 case。
- **Verify**: CPP(Spec182OnnxWorkerProtocol/*)；纯 parser/state 的截断/溢出/重复帧和晚到结果；L0 安装链接。真实子进程 cleanup 在 T016。

### T006-D Protected Provider Activation

- **Parent**: T006; **Depends**: T006-C; **Reviewer**: security review
- **Read**: Assembly OA09/S8 → CD-005；N/NativeCanonicalOnnxAssembler.cpp::prepareNativeCanonicalOnnxRole；N/ProtectedRuntime.cpp。
- **Write**: N/NativeCanonicalOnnxAssembler.hpp; N/NativeCanonicalOnnxAssembler.cpp; A/onnx/NativeOnnxRecipeAssembler.cpp; U/di-native-onnx-recipe.t.cpp; I/di-native-onnx-recipe.t.cpp。
- **Steps**: 替换 helper 调用为同库 worker，移除旧文件 IPC/Python 参数，保留签名/cache/secret lease；父端重新核验结果 hash/权限后才激活。
- **Verify**: CPP(Spec182OnnxActivation/*)；固定 manifest、错 hash/取消后不得签名激活；编写 Selection 后真实冷装配 case，T016 执行。

### T007-A Full Tokenizer Ownership

- **Parent**: T007; **Depends**: T002-A; **Reviewer**: ABI/lifetime review
- **Read**: Tokenizer → CD-006；F/dependency-probes/tokenizer/tokenizer-abi.h、src/lib.rs、Cargo.lock；N/NativeStandaloneTokenizer.cpp 的旧完整 decoder。
- **Write**: A/qwen/tokenizer-bridge/Cargo.toml; A/qwen/tokenizer-bridge/Cargo.lock; A/qwen/tokenizer-bridge/src/lib.rs; A/qwen/tokenizer-bridge/tokenizer-abi.h; N/NativeTokenizer.hpp; N/NativeTokenizer.cpp; N/NativeStandaloneTokenizer.hpp; N/NativeStandaloneTokenizer.cpp; U/di-native-tokenizer.t.cpp; wscript。
- **Steps**: 迁入锁定 HF 核心的五函数 ABI，C++ digest/RAII/mutex 与完整 encode/decode factory；同 allocator 释放，不升级 crate 或安装 probe 库。保留原完整 factory 兼容签名。
- **Verify**: CPP(Spec182TokenizerFull/*)；84 完整对照、special flags、错误 ID/UTF-8/digest、owner 复用与串行调用；实际 native ABI 为被测对象。

### T007-B Stable Text Decoder Pair

- **Parent**: T007; **Depends**: T007-A; **Reviewer**: stream/ABI review
- **Read**: Stream API and Ownership、Stable Prefix Algorithms、Production Caller Inventory → Tokenizer；F/dependency-probes/check-bytelevel-stream.py、check-stream-boundaries.py。
- **Write**: A/qwen/tokenizer-bridge/src/lib.rs; A/qwen/tokenizer-bridge/tokenizer-abi.h; N/NativeTokenizer.hpp; N/NativeTokenizer.cpp; N/NativeStandaloneTokenizer.hpp; N/NativeStandaloneTokenizer.cpp; N/NativeProviderHandler.hpp; U/distributed-inference-tokenizer.t.cpp。
- **Steps**: 新增第六私有 stable ABI、同摘要 paired factory 和 handler factory 声明；分别实现已冻结 ByteLevel/ByteFallback profile 与 final flush。生产注入由 T009-C/T011-B 完成，避免 CLI/host 两份逻辑。
- **Verify**: CPP(Spec182TokenizerStable/*)；合法 U+FFFD、截断/非法 bytes、special、交错调用、final/full 一致；拒绝未知 stream profile，不猜前缀稳定性。

### T008-A Native Input and Artifact Preparation

- **Parent**: T008; **Depends**: T003-C, T006-D, T007-B; **Reviewer**: adapter/identity review
- **Read**: CD-013 → Symbols/Values；P/artifact_deployment.py::CanonicalCatalogEnsurer；P/adapters/base.py 的 GraphAdapter/TaskAdapter。
- **Write**: N/NativeRequestPreparation.hpp; N/NativeRequestPreparation.cpp; N/NativePlanning.hpp; A/qwen/NativeQwenPlanner.hpp; A/qwen/NativeQwenPlanner.cpp; A/yolo/NativeYoloPlanner.hpp; A/yolo/NativeYoloPlanner.cpp; U/di-native-preparation.t.cpp; I/di-native-preparation.t.cpp; wscript。
- **Steps**: 实现 inspect/encodeInput/decodeResult 与 prepareInput/inspectModel/ensureArtifacts 的冻结端口，复用两 adapter；认证 name/digest 绑定，I/O 位于 preparation，不移入纯策略。
- **Verify**: CPP(Spec182Preparation/*)；两模型 input/result mapping、错 catalog/publication name/digest、清理边界；真实 publication 在 T016。

### T008-B Authenticated Offer Admission

- **Parent**: T008; **Depends**: T008-A; **Reviewer**: provenance/security review
- **Read**: CD-013 → CD-002 snapshot；P/app_sdk/provider.py::ProviderOfferTrustVerifier；ndn-service-framework/ServiceUser.hpp 的 ACK provenance。
- **Write**: N/NativeOfferAdmission.hpp; N/NativeOfferAdmission.cpp; N/NativeObservedOfferV3.hpp; N/NativeObservedOfferV3.cpp; U/di-native-observed-offer.t.cpp; tests/fixtures/spec182/offer-python-oracle.json; tests/fixtures/spec182/author-offer-python-oracle.py; U/di-native-offer-admission.t.cpp; I/di-native-preparation.t.cpp; wscript。
- **Steps**: 先将 ACK payload 解码为无认证权力的 NativeObservedProviderOfferV3，保留真实 topology/resources/residency 与 SDK canonical digest；再使用 Core 认证结果检查 policy/有效期/绑定及 policy-bound offer signature，形成 immutable view。policy 不提供 Provider 观测能力；拒绝 caller trusted=true 和 Python verifier callback。
- **Verify**: CPP(Spec182OfferAdmission/*)；伪 provenance、错身份/策略、过期 ACK；无合法 view 就不能进入 strategy；真实 Core admission 留 T016。

### T009-A Core Scoped Registration

- **Parent**: T009; **Depends**: T006-D, T007-B; **Reviewer**: lifecycle/security review
- **Read**: Host Registration Generation Decision、Provider Lifetime Control、Request and Collaboration State Ownership → ndn-service-framework/ServiceProvider.hpp/.cpp 的注册、ACK、Selection、work fence、cleanup。
- **Write**: ndn-service-framework/ServiceProvider.hpp; ndn-service-framework/ServiceProvider.cpp; U/di-native-provider-host.t.cpp。
- **Steps**: 增加 service-neutral scoped registration RAII、RegistrationControl 和 pending/协作代次绑定；保留 legacy API，覆盖同步 fallback、晚到结果、锁外 capture 析构。不可引入 DI 类型或新 wire。
- **Verify**: CPP(Spec182Registration/*)；包含 Host 冻结的六个 Registration case；通过真实 Core dispatch/worker/Face 检查，不以独立 bool 代替，跨服务 NFD 用例留 T016。

### T009-B Shared Execution Lease State

- **Parent**: T009; **Depends**: T009-A; **Reviewer**: resource/security review
- **Read**: Host Shared Lease Ownership、Closing Targets → N/ExecutionLeaseService.hpp/.cpp::handle/table；ndn-service-framework/ExecutionLease.hpp。
- **Write**: N/ExecutionLeaseService.hpp; N/ExecutionLeaseService.cpp; U/di-native-provider-host.t.cpp。
- **Steps**: 增加 shared table/prepare mutex constructor，保留原 constructor；非Prepare操作增加 target 绑定检查，复用 Core 身份/状态/重放验证；关闭不能提前释放执行中槽。
- **Verify**: CPP(Spec182SharedLease/*)；Host 三个 SharedLease/Closing case，两个 target 争同槽、跨 target 操作拒绝及旧单 target 回归。

### T009-C Shared Provider Host Wiring

- **Parent**: T009; **Depends**: T009-B; **Reviewer**: lifecycle/security review
- **Read**: Host → CD-014 → Stream caller inventory；examples/DI_NativeProviderExecutable.cpp::main；N/NativeProviderHandler.hpp；ndn-service-framework/ServiceProvider.hpp。
- **Write**: N/NativeInferenceProvider.hpp; N/NativeInferenceProvider.cpp; examples/DI_NativeProviderExecutable.cpp; U/di-native-provider-host.t.cpp; I/di-native-provider-host.t.cpp; wscript。
- **Steps**: 按冻结 O-004 抽取一个共享宿主、lease 表和 factory 注入，serve/close/stop fence 属于同一行为单元；保留 shared Face、管理权限隔离和 generation 验证。CLI 仅构造配置并调 host。
- **Verify**: CPP(Spec182ProviderHost/*)；双 target、duplicate/close/re-register、late ACK/Selection、旧 lease 清理不会影响新注册；真实 NFD 多入口在 T016。

### T010-A Request Operation Terminal State

- **Parent**: T010; **Depends**: T005-B, T008-B, T009-C; **Reviewer**: lifetime/concurrency review
- **Read**: CD-001 → runtime-boundaries Cancellation and Observer Contract → Symbols/Values；ndn-service-framework/ServiceUser.hpp 的 collaboration handle。
- **Write**: N/NativeInferenceClient.hpp; N/NativeInferenceClient.cpp; ndn-service-framework/ServiceUser.hpp; ndn-service-framework/ServiceUser.cpp; U/di-native-client.t.cpp; wscript。
- **Steps**: 实现 handle/operation owner、poll/result/cancel/deadline/observer 单一终态；注入冻结 Core port，不在 Face 线程等待，不允许 late callback 复活。
- **Verify**: CPP(Spec182ClientState/*)；cancel 与成功竞争、绝对 deadline、重复/晚到回调、共享资源销毁；无完整 request stub。

### T010-B Complete Request Orchestration

- **Parent**: T010; **Depends**: T010-A; **Reviewer**: production wiring review
- **Read**: CD-001 FLOW-001/FLOW-002 → CD-013；P/app_sdk/placement.py::_request_v3；前置 native planner/sealer/grant/preparation/admission API。
- **Write**: N/NativeInferenceClient.cpp; examples/DI_NativeRequester.cpp; examples/wscript; U/di-native-client.t.cpp; I/di-native-request.t.cpp。
- **Steps**: 接 model/input→ACK_CLOSED→prepare/admit→split/place→seal/grant→commit→Response；CLI 只处理公开参数。保留每个首拒绝边界，不能只实现 preplanned 快捷路径。
- **Verify**: CPP(Spec182ClientRequest/*)；冻结 port 输入/输出与拒绝后无 commit；编写真实 Core/Provider 完整请求 case，T016 运行。

### T010-C Stream Acceptance and Replacement

- **Parent**: T010; **Depends**: T010-B; **Reviewer**: stream/recovery review
- **Read**: Stream Requester Acceptance and Recovery Resolution → CD-001；P/app_sdk/placement.py::AutomaticStreamingHandle；N/NativeInferenceClient.cpp 的 operation。
- **Write**: N/NativeInferenceClient.cpp; U/distributed-inference-stream-recovery.t.cpp; I/di-native-request.t.cpp。
- **Steps**: 在同一 operation 实现 accept/replacement/final 验证，保持已接受 token/text 前缀；terminal 后禁止重启，最终文本与已接受增量一致。
- **Verify**: CPP(Spec182StreamAcceptance/*)；重复/缺序/错 lineage、replacement replay、错 final、terminal 后恢复拒绝；不把内存接受点当逐 token 持久 journal。

### T011-A Sampling Parity Repair

- **Parent**: T011; **Depends**: T010-C; **Reviewer**: numeric review
- **Read**: Sampling → N/NativeEpochCoordinator.cpp::sampleToken/deterministicUnit；P/adapters/qwen/generation.py::sample_token；S/evidence/native-reuse-review-20260906.md 的独立输入。
- **Write**: N/NativeEpochCoordinator.cpp; U/di-native-conversation.t.cpp。
- **Steps**: 修复去重 penalty、Top-P retained mass、double precision 与参数验证；保留 SplitMix64、epoch 与旧 prefix mismatch 规则，不另建生成 runtime。
- **Verify**: CPP(Spec182Sampling/*)；suite 内注册 Sampling 契约的四个具名 case，通过真实 epoch 调用私有 sampler；float32 输入下 seed8/top_p0.8 及重复 penalty 两例均应选0，expected 来自旧 reference。

### T011-B Stable Epoch Emission

- **Parent**: T011; **Depends**: T011-A; **Reviewer**: stream/commit review
- **Read**: Stream Epoch Integration and Recovery Boundary、Production Caller Inventory → N/NativeEpochCoordinator.cpp::runNativeEpochCoordinator；N/NativeProviderHandler.cpp 的 authenticated generation config。
- **Write**: N/NativeEpochCoordinator.hpp; N/NativeEpochCoordinator.cpp; N/NativeProviderHandler.cpp; N/NativeInferenceProvider.cpp; U/distributed-inference-stream-recovery.t.cpp; I/ndnsf-di-core-flow.t.cpp。
- **Steps**: 同摘要 pair 传给 coordinator；完整文本判断 stop 后 preview stable/final flush，再按原 event/feedback/state 顺序推进。拒绝事件丢候选；已接受事件不能靠 decoder rollback 撤回。
- **Verify**: CPP(Spec182EpochText/*)；stable delta、EOS/MAX_TOKENS/stop、cancel/event rejection、接受后反馈失败与 replay 不重复事件；三个真实 integration caller 同步编写，T016 运行。

### T011-C Conversation Journal and Continuation

- **Parent**: T011; **Depends**: T011-B; **Reviewer**: state/compatibility review
- **Read**: CD-007 State Authority → runtime-boundaries Migration and Rollback Contract；P/conversation.py::ConversationCoordinator；P/app_sdk/runtime_journal.py；N/NativeProviderRuntime.hpp::ConversationStateStore。
- **Write**: N/NativeConversationCoordinator.hpp; N/NativeConversationCoordinator.cpp; N/NativeInferenceClient.hpp; N/NativeInferenceClient.cpp; U/di-native-conversation.t.cpp; I/di-native-conversation.t.cpp; wscript。
- **Steps**: 实现 begin/abort/prepareCheckpoint/commit/restore 和 client 接线；Requester journal 与 Provider KV owner 分离，旧格式按冻结兼容处置，单次受支持 replacement 有界。
- **Verify**: CPP(Spec182Conversation/*)；wrong-parent、lineage/prefix、cancel、旧 journal/重复 commit；编写真实两轮续接与恢复，T016 执行。

### T012-A Native Binding Types and Lifetime

- **Parent**: T012; **Depends**: T011-C; **Reviewer**: GIL/ABI/lifetime review
- **Read**: CD-008/CD-009 → Symbols/Values → runtime-boundaries Cancellation and Observer Contract；pythonWrapper/src/ndnsf/_ndnsf.cpp 模块注册；前置 native public headers。
- **Write**: pythonWrapper/src/ndnsf/di_bindings.cpp; pythonWrapper/src/ndnsf/_ndnsf.cpp; pythonWrapper/setup.py; tests/python/test_spec182_native_bindings.py。
- **Steps**: 注册同库类型/错误、值转换、handle/provider 所有权和 GIL 边界；拒绝 Python strategy trampoline，不编译第二份 DI source。
- **Verify**: PY(tests/python/test_spec182_native_bindings.py, unit_binding)；异常、取消、destroy/observer 生命周期和 callback 拒绝；必要链接核对；真实请求 parity 留 T016。

### T012-B Compatible Python Facades

- **Parent**: T012; **Depends**: T012-A; **Reviewer**: compatibility review
- **Read**: CD-008 → S/contracts/compatibility-manifest.json；P/app_sdk/application.py、client.py、provider.py 的公开方法。
- **Write**: P/app_sdk/application.py; P/app_sdk/client.py; P/app_sdk/provider.py; P/__init__.py; P/app_sdk/__init__.py; tests/python/test_spec182_native_bindings.py。
- **Steps**: 按完整调用清单转发 native binding，保持参数/异常/返回契约；缺 binding 时明确失败，不能 fallback 旧控制器或在 Python 判定成功。
- **Verify**: PY(tests/python/test_spec182_native_bindings.py, unit_facade)；固定方法签名、参数映射、无 Python planner 调用；真实 C++/Python 一致性由 T016。

### T013-A Maintained Caller Migration

- **Parent**: T013; **Depends**: T012-B; **Reviewer**: caller coverage review
- **Read**: CD-010 → compatibility-manifest 的 maintained callers；原三个 MiniNDN harness 和四个示例的入口。
- **Write**: Experiments/NDNSF_DI_YoloAckDriven_Minindn.py; Experiments/NDNSF_DI_QwenAckDriven_Minindn.py; Experiments/NDNSF_DI_StreamedGeneration_Minindn.py; examples/python/NDNSF-DistributedInference/yolo_2x2/user.py; examples/python/NDNSF-DistributedInference/yolo_2x2/provider.py; examples/python/NDNSF-DistributedInference/llm_pipeline/user.py; examples/python/NDNSF-DistributedInference/llm_pipeline/provider.py; tests/python/test_spec182_legacy_exclusion.py。
- **Steps**: 按清单切换 native executable/binding，保留参数/oracle/清理；额外 maintained caller 如未在 Write 登记，先回 T001-C 补精确范围再分派。
- **Verify**: PY(tests/python/test_spec182_legacy_exclusion.py, unit_routes)；入口/参数构造与默认路由单测，网络 harness 只编写不运行。

### T013-B Legacy Runtime Retirement

- **Parent**: T013; **Depends**: T013-A; **Reviewer**: migration/reachability review
- **Read**: CD-010 → runtime-boundaries Migration and Rollback Contract → compatibility-manifest；CodeGraph 与 import/callback 清单核对旧 owner。
- **Write**: P/provider.py; P/runtime_v1.py; P/app_sdk/facades.py; P/app_sdk/placement.py; tests/python/test_spec182_legacy_exclusion.py; S/contracts/compatibility-manifest.json。
- **Steps**: 仅删除清单确认无生产 consumer 的旧运行实现；保留已登记离线用途，不能搬到工具层继续默认调用；其他清单路径必须先由 T001-C 展开 Write，禁止通配删除。
- **Verify**: PY(tests/python/test_spec182_legacy_exclusion.py, unit_retirement)；禁止旧模块时默认 import/路由不回退；实际维护入口运行与无 Python 证明留 T016。

### T014-A Isolation Collector Semantics

- **Parent**: T014; **Depends**: T013-B; **Reviewer**: evidence-validity review
- **Read**: Isolation 的函数/字段/I01--I08 → Proof Negative Path Matrix；既有 Spec181 closure collector 仅作结构参考。
- **Write**: tests/standalone/run-spec182-native-closure.py; tests/python/test_spec182_native_closure.py; F/case-manifest.json。
- **Steps**: 实现 manifest/身份校验、进程/映射/endpoint 解析与首边界 verdict；解析器和 runtime 共享一套判定，不伪造 PASS marker；观测不完整拒绝给资格。
- **Verify**: PY(tests/python/test_spec182_native_closure.py, unit_collector)；固定独立 trace fixtures、短命 libpython/重命名 helper/缺事件/错身份；真实隔离反例留 T016。

### T014-B Qualification Harness Registration

- **Parent**: T014; **Depends**: T014-A; **Reviewer**: harness/oracle review
- **Read**: Isolation → Proof Acceptance Cases；Experiments/NDNSF_DI_YoloAckDriven_Minindn.py、NDNSF_DI_QwenAckDriven_Minindn.py 的启动/清理；T014-A collector API。
- **Write**: Experiments/NDNSF_DI_NativeClosure_Minindn.py; tests/standalone/run-spec182-native-closure.py; tests/python/test_spec182_native_closure.py; F/case-manifest.json。
- **Steps**: 接同一 collector，登记完整正负例、fresh run-dir、绝对 deadline 与仅本次资源清理；harness Python 留 scope 外，NFD/Repo/Controller 白名单仍核对真实依赖。
- **Verify**: PY(tests/python/test_spec182_native_closure.py, unit_harness)；命令/manifest/cleanup 决策 unit，不启动 namespace/网络；I01--I08 与全部 PO 用例可由 T016 实际运行。

### T015-A CrossTask Convergence

- **Parent**: T015; **Depends**: T014-B; **Reviewer**: design owner
- **Read**: S/traceability.md → 父任务结果及变更契约 → 真实 requester/provider/binding/collector 调用链；按 CD/FR/PO 定向读源码。
- **Write**: S/audit.md; S/traceability.md。
- **Steps**: 审查所有已交付卡的接线、effective config、依赖/fixture 身份与未关闭义务；缺陷回所属卡修复再审，不自行放宽设计。局部 unit PASS 不能替代整体语义判断。
- **Verify**: DOC 与全链静态语义审查；控制性发现清零才允许 T016，结构 validator 不授予此 PASS。

### T016-A Local Qualification

- **Parent**: T016; **Depends**: T015-A; **Reviewer**: qualification review
- **Read**: Proof → Isolation → T015 当前 PASS 及 source/dependency/config/harness identity；F/case-manifest.json。
- **Write**: S/evidence/t016-completion.md。
- **Steps**: 按冻结 manifest 分 case 顺序运行完整 unit/integration/MiniNDN/no-Python；每次新 raw run，记录全部正负例。失败先分类并交所属卡修复，回归受影响范围和重新审查；不接 SIF/Tiger。
- **Verify**: FINAL；所有 PO 与 Negative Path Matrix 用真实边界证据关闭，启动/collector/身份失败不得算预期协议拒绝。

### T017-A Development Handoff

- **Parent**: T017; **Depends**: T016-A; **Reviewer**: delivery review
- **Read**: CD-012 → S/plan.md Delivery → Experiments/TigerCluster/docs/source-handoff.md；T016 actual evidence。
- **Write**: S/evidence/development-handoff.md; docs/architecture.md; docs/ndnsf-core-app-boundary.md; docs/NDNSF-DI-runtime-workflow.md。
- **Steps**: 按 CD-012 核对唯一源码/库/依赖与实例，复用现有交付入口；C++/Python 最短示例引用已验收入口。若 CD-012 清单与上述文档不同，先在 T001-C 更新此卡，不能猜交付路径。
- **Verify**: DOC；交付 hashes、示例调用、有效 T016 证据和可复现命令一致；外部 SIF/Tiger 标 TRANSFERRED，无行为变化不重跑套件。

## Completion and Progress

每个工作单元结束时更新 [Execution Progress](../tasks.md#execution-progress) 的状态、证据、剩余验收和日期。
提交和回复前核对；卡片保留执行定义，不维护第二份状态。新增、拆分、补救和验证任务先补稳定 ID、卡片和进度行。
父任务全部子任务 DONE 且原验收满足后才勾选；T016 正式资格验收保持独立。
按已授权范围逐单元推进，换模型、换会话不丢失剩余工作。
通用规则见 [task progress](../../../skills/speckit-code-design/references/task-progress.md)。
