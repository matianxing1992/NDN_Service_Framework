# Failure Log and Evidence Index

## 2026-09-07 — Spec182 typed shape Provider consumers

r2 构建在 NativeProviderHandler.cpp 的 YOLO 文本 metadata 与 decode tensor identity
两个旧字符串消费者失败。已改显式文本转换及带类型的身份编码；原始 r2 build.log 保留。
见 [typed shape evidence](../specs/182-native-di-python-bindings/evidence/t004-typed-shape-20260907.md)。

## 2026-09-07 — Spec182 typed shape consumer compilation

shape variant 的新 ABI 构建 r1 在 NativeYoloMergeRunner.cpp:335 失败：旧 metadata
字符串拼接未处理整数/符号类型。已定位文本 runner 配置边界，显式转换后独立 r2 重试。
见 [typed shape evidence](../specs/182-native-di-python-bindings/evidence/t004-typed-shape-20260907.md)。

## 2026-09-07 — Spec182 canonical float overflow probe

typed canonical JSON r1 的 DBL_MAX 对照失败：stream 解析溢出后饱和值误判为回转成功，
输出 2e+308。已定位为 helper 缺少 failbit 检查；保留 oracle，修复后独立 r2 重试。
见 [canonical JSON evidence](../specs/182-native-di-python-bindings/evidence/t004-canonical-json-20260907.md)。

## 2026-09-07 — Spec182 T003 placement repair fixture failures

r1 定向测试 exit 201：sealer fixture 依赖无关 residency 排序；新双角色 fixture 少传一个
tensor degree，在 splitter 构造阶段拒绝。已定位 fixture 首边界，修复输入后在独立 r2 重试。
原始 `.codex-tmp/spec182-t003-placement-r1/` 保留；见
[placement repair](../specs/182-native-di-python-bindings/evidence/t003-role-placement-20260907.md)。

## 2026-09-07 — Spec182 T003 placement source audit reopened

NativePlanning.cpp::propose 将全部角色分配给一个 Provider，并按 residency 集合大小
代替目标工件命中排序。既有 rank-one 局部 PASS 未覆盖该缺陷；T003-C 及相关依赖 DONE
已回退 PARTIAL。此为源码审计，没有新的失败运行；下一步修复逐角色独立 Provider 分配。
同时 T004 真实工件/grant 输入修复已完成定向 33-case 验证，完整 wire 仍未完成。
见 [binding repair and placement audit](../specs/182-native-di-python-bindings/evidence/t004-artifact-grant-bindings-20260907.md)。

## 2026-09-07 — Spec182 T004 native Selection wire incompatible

最小 native codec 诊断中，NativePlanSealer::encode 接受字段并输出 386 字节，
生产 nativeSelectionProjectionV3FromJson 在第一 schema 门拒绝。首边界是 DI wire
构造，不是 Core 网络或运行环境。T004-A/父 T004 重开，禁止把旧 7-case PASS 当完整
Selection 证明；修复完整 canonical wire 和真实工件/grant 绑定后再继续 T010。
原始 `.codex-tmp/spec182-t004-wire-audit-r1/` 保留；见
[A8-01 evidence](../specs/182-native-di-python-bindings/evidence/t004-wire-reopened-20260907.md)。

## 2026-09-07 — Spec182 T010-A unit selector rejected

两个 Boost.Test suite 误用逗号组合，exit200，`no test cases matching filter or all
test cases were disabled`；未执行用例，不是 requester 行为失败。原始
`.codex-tmp/spec182-t010a-deadline-r1/client-state.log` 保留，改为分别执行单 suite。
见 [deadline evidence](../specs/182-native-di-python-bindings/evidence/t010-a-deadline-20260907.md)。

## 2026-09-07 — T002-A L0 consumer aborts: freeze() rejects an empty native adapter registry
- **Area**: spec182 T002-A Installed Library Boundary
- **Symptom**: installed-library consumer
  `tests/standalone/spec182-installed-consumer.cpp` (T001-C-frozen L0 carrier)
  ran against the staged prefix and aborted before printing its OK marker:
  `terminate called after throwing an instance of 'std::invalid_argument'`,
  `what(): native adapter registry is empty`, RUN_RC=134 (core dumped).
- **Root cause**: intra-spec182 contradiction. Commit `3afa7492`
  (2026-09-07 01:50, “spec182: add native assembly grants preparation and
  bindings”) *introduced* `NativeAdapterRegistry::freeze()` together with an
  empty-registry precondition, eight minutes before commit `aba90194`
  (01:58) froze `spec182-installed-consumer.cpp` as the L0 carrier, whose
  probe semantics require an empty registry to be freezable
  (`frozen()==true`, `find("missing")==nullptr`). No production caller
  depends on the throw today; the only prior freeze caller
  (`tests/unit-tests/di-native-preparation.t.cpp:51-52`) registers an
  adapter first. At the T002-A stage the installed library deliberately has
  no concrete adapter class yet (T003-A/B add them later), so rejecting the
  empty state makes the installable boundary unusable by any consumer.
- **Fix**: `freeze()` now latches unconditionally; the “at least one
  adapter” precondition, where a caller needs it (e.g. a provider), is
  enforced at the use site by a `find()`-null check, not by the registry
  latch. Evidence records the re-run under the frozen L0 command.
- **Ref**: NDNSF commit `3afa7492` / `aba90194`; run dir
  `.codex-tmp/spec182-t002a-l0-r1/` retained.
- **Lesson**: a frozen executable carrier and its own feature's earlier
  implementation commits can disagree; the later process freeze wins, and
  every frozen carrier must actually be executed once before closure.

## 2026-09-07 — Spec182 tokenizer bridge toolchain unavailable

尝试在本机对固定 Rust tokenizer bridge 做 release 构建时，首边界为
`cargo: command not found`（exit127）；未进入 Cargo 解析、编译或链接，不能把
bridge/ABI 记为通过。保留原始目录 `.codex-tmp/spec182-tokenizer-r1/`；现有
C++ tokenizer 包装和静态检查继续作为源码证据，实际 bridge 构建需在安装了
Rust1.90/Cargo 的同源工具链上重跑。

同一诊断目录的首次手工 Boost.Test 链接遗漏 `-pthread`，`libcrypto` 因此出现
`pthread_*` 未解析；补齐线程库后同一 3 个负例用例 **3/3 PASS**。该次失败是
命令边界，不是 tokenizer 实现或测试失败，原始摘要见
`.codex-tmp/spec182-tokenizer-r1/link-r1.json`。

## 2026-09-07 — Spec182 skill validator schema mismatch

Spark执行包检查时，通用skill quick_validate拒绝两个既有Spec Kit入口的顶层`compatibility`字段；首边界为校验器schema，不是任务执行或native产品失败。保留原格式，YAML/必需字段/profile路由检查PASS；仓库code-design通用校验PASS。实际检查和fallback见[执行包记录](../specs/182-native-di-python-bindings/evidence/spark-execution-preparation.md#validation)。未启动产品构建/测试。

## 2026-09-07 — Spec182 installable DI library build blocked at NAC-ABE ABI

构建 `ndnsf-distributed-inference` 在既有 `ndn-service-framework` 编译边界失败，exit1；`ServiceUser.cpp`/`ServiceProvider.cpp` 调用 `getPublicParamsDataName`、`getPublicParamsDigest`、`clearCache`、`refreshPublicParameters`、`refreshDecryptionKey`，当前安装的 NAC-ABE 头文件没有这些成员。该失败发生在 DI 新对象编译前，不能归因于 Spec182 代码，也不能把本次构建当作库或产品 PASS。原始命令/首边界记录在 `.codex-tmp/spec182-native-build-20260907-r1/`；下一步先固定匹配的 NAC-ABE 头/库工具链，再重跑同一目标。

## 2026-09-06 — Spec182 typed-complex reference conversion

扩展initializer参考提取R1在complex64-typed失败：ONNX1.17 `_to_array`先组合complex值，再以float storage dtype调用np.asarray，抛`TypeError: can't convert complex to float`，exit1。此前模型full checker已通过，尚未产生identity；不是C++算法失败。raw `.codex-tmp/spec182-t001-identity-extended-r1/boundary.json`。下一R2分别提取其他表示并将typed-complex单独记为旧转换缺陷，不把它标成已支持稳定oracle或降低普通numeric验收范围。

## 2026-09-06 — Spec182 native reuse review boundaries

审计发现现有NativeEpochCoordinator将完整decode作为稳定stream前缀，且C++采样的Top-P截断归一化、重复token惩罚与Python reference不一致。固定tokenizers0.20.3/现有byte-fallback fixture诊断exit0：`好`的prefix为`�→��→好`；seed8的Top-P例和重复token Greedy例，Python返回0、native源代码推导为1。首边界是文本提交/采样算法，不是网络或授权失败；未执行native产品。raw `.codex-tmp/spec182-native-reuse-review-20260906-r1/`；完整输入/hash/源码/边界见[native reuse review](../specs/182-native-di-python-bindings/evidence/native-reuse-review-20260906.md)。A7-08/A7-09 OPEN，T001/O-004先冻结处置，T007/T011修复后由T016验收，不将reference诊断计为产品PASS。

## 2026-09-06 — Spec182 legacy ONNX initializer identity boundary

T001独立reference探针（ONNX1.17.0/NumPy1.24.4，未修改graph.py）确认：BFLOAT16 raw_data两次计算的content digest均不等于声明权重位模式，而typed表示正确；STRING相同model digest在两个独立进程产生不同initializer content digest。首边界是旧numpy_helper/object-array归一化，不是网络、授权或原生装配结果。普通12种数值类型的24个raw/typed向量通过。raw `.codex-tmp/spec182-t001-identity-r1/diagnostics.json`，脱敏[durable evidence](../specs/182-native-di-python-bindings/evidence/identity-reference-20260906.json)。O-002/O-004继续冻结稳定身份与兼容处置；不把错误摘要写为正确oracle、不以未运行的C++测试关闭此缺陷。

## 2026-09-06 — Spec182 tokenizer toolchain download transport

R2改为rustc/cargo/rust-std最小组件，首个rustc归档仍在Python3.8 urllib TLS读取阶段以同样错误exit1；保留`rust-r2/boundary.json`与部分归档。R3只切换Node22 HTTPS传输，保持官方来源、TLS验证及SHA256检查，最多一次有界重试；依赖设计与其他T001工作继续，不把下载失败升格为产品阻塞。

R3已恢复：Node22 HTTPS成功下载rustc/cargo/rust-std三组件，官方SHA256全部匹配，exit0。失败边界为旧Python传输路径，未观察到tokenizer代码问题；hash与后续探针结果记录在同一dependency design。

T001依赖可行性所需Rust1.90.0归档下载R1 exit1，在TLS body读取阶段报`DECRYPTION_FAILED_OR_BAD_RECORD_MAC`，尚未校验/解压/安装，更未编译或执行tokenizer。保留部分归档与`.codex-tmp/spec182-t001-dependencies/rust-r1/boundary.json`；新R2目录有限重试并验证官方SHA256，不使用部分文件。ONNX依赖构建独立，不因下载失败重跑。设计及进度见[dependency design](../specs/182-native-di-python-bindings/contracts/native-dependency-design.md)。

## 2026-09-06 — Delivery-only scope correction

用户明确指出本轮任务仅为交付，编译与测试由另一台机器负责。此前本机ABI消费者验证属于超出范围的扩展；立即停止R4 owned构建进程组2531869，不再启动NDNSD构建、unit/integration、Python扩展验证或MiniNDN。R1/R2中断及R3普通Provider构建记录保留，不能外推完整验证PASS。后续构建/测试均TRANSFERRED，不作为交付阻塞项；当前源码包/definition/依赖锁/skills与GitHub发布已完成，见source handoff。

## 2026-09-06 — Waf interrupted signature persistence

R2日志证明R1超时后Waf未保存task signatures，实际从1/318重新编译，不能称为仅续编剩余对象。停止重复R2（SIGINT exit68，78.516s）并确认编译子进程退出，保留同一raw root的`build-r2/boundary.json`。R3以原配置、`-j2`、3600秒上限只选择缺失的`di-native-provider`及必要依赖；R1在同一fresh树已经成功链接的Core/unit/integration/应用保留逐目标证据。最终需补齐全部交付目标并运行测试，不将任何中断轮次记PASS。

## 2026-09-06 — Fresh ABI build runner time limit

新SVS/NDNSD闭包消费者fresh build R1在299/318触发执行器1800秒上限：exit124，1800.081s，`TIMEOUT_AT_RUNNER_BOUNDARY`。没有compiler error，Core/unit/integration及部分应用已链接，仍有native-provider对象未完成；不能把未完整构建记PASS或解释为协议失败。独立验证树 `/home/tianxing/NDN/ndnsf-svs-abi-20260906` 下 `.codex-tmp/svs-abi-20260906-r1/build-r1/` 保留receipt、boundary和完整log；确认无遗留编译进程后以 `build-r2`、3600秒有限上限继续同一fresh `build-abi`，配置和`-j2`不变。

## 2026-09-06 — Source handoff tooling resolved

最终交付工具/模板/旧builder fixture统一R5 **28/28 PASS**；五项共享skill与接收说明链接/语法通过。真实四库包生成、搬迁后verify、固定base摘要及definition render PASS。R1/R2的生成物/离线VERSION.info、canonical workload与历史host-gate fixture边界均已定位并修复，原日志保留。详见 [source handoff checkpoint](../Experiments/TigerCluster/docs/source-handoff.md#checkpoint)。NDNSF新SVS/NDNSD ABI闭包的fresh编译与运行验证仍待完成，不宣称SIF或Tiger PASS。

## 2026-09-06 — SVS offline metadata and transitive ABI closure

真实归档R2在 `LOCAL_SIF_DEPENDENCY_SOURCE_MISSING:VERSION.info` 停止（exit1），raw `.codex-tmp/source-handoff-20260906/package-r2.log`。SVS的该文件由Waf生成且被Git忽略，本机残留版本仍指向旧commit。改为从固定源码的VERSION/GIT_TAG_PREFIX和git describe生成归档内元数据，记录派生来源，不修改源checkout。同时静态查到NDNSD自己构造SVSPubSub，旧NDNSD二进制也是ABI消费者；将其干净源码及pkg-config路径修复纳入锁定和fresh重建，不复用base中的旧库。

旧build-record fixture R2进一步在 `SPEC175_WORKLOAD_NOT_SEALED:Experiments/TigerCluster/jobs/spec175/workload.json` 拒绝，raw `.codex-tmp/source-handoff-20260906/source-handoff-build-record-r2.log`：兼容alias与canonical路径不一致。fixture按canonical封装，真实sealer同时保留legacy镜像路径及canonical身份，生产门保持不变。

## 2026-09-06 — Source archive cleanliness boundary

真实交付包R1在 `HANDOFF_SOURCE_UNTRACKED:examples/example-trust-anchor.cert` 拒绝，未创建bundle：开发依赖checkout带有未跟踪生成物，HEAD与tracked clean不足以证明归档内容。保留原目录，改为三库全部使用精确commit的全新detached checkout准备R2，不放宽sealer的未跟踪源码检查。这是输入来源失败，尚未运行容器编译。

## 2026-09-06 — Source handoff tool fixtures

交付工具统一检查R1为19 PASS / 5 FAIL，原始 `.codex-tmp/source-handoff-20260906/tool-checks-r1/output.log` 保留。五项旧 `test_build_local_sif_record.py` 都在 `HOST_GATE_WORKLOAD_SEED_MISMATCH` 提前退出：fixture引用历史真实G3清单，但运行时校验当前workload。未到达所测definition/label/source边界，不是新SIF构建失败。修复测试为独立临时fixture，保留生产门与负例；不更新历史资格清单冒充当前结果。

模板首轮静态检查曾因开头注释被既有boundary parser识别成第三stage而失败；说明移入builder头之后。随后静态复审发现wheel锁只检查非空会漏掉离线python-ndn依赖，现要求五个固定wheel输入并补缺项拒绝。模板原始R1/R2日志由本轮 [source handoff](../Experiments/TigerCluster/docs/source-handoff.md) 记录；这些静态/fixture失败均未执行Apptainer。

## 2026-09-06 — Merge validation resolved

静态修复后 full unit 759/759、GDB full integration 154/154、current Python 2171 passed /22 skipped；MiniNDN用户撤销、仅新增授权、Provider撤销全部 PASS。PATH启动失败在独立 R2 修复，最初日志不覆盖。Provider场景主动 SIGINT 后重启的旧进程 exit -2，其余应用 exit0。完整身份和范围见 `specs/182-native-di-python-bindings/evidence/merge-validation-20260906.json`；不能把 current Python 范围或三个网络场景外推为历史全套/181最终qualification。

## 2026-09-06 — MiniNDN launcher PATH boundary

合并验证 `minindn-user-revocation-r1` 在 0.644 s 退出1，首边界为 Mininet 启动器找不到 `ifconfig`；编译 PATH `/usr/bin:/bin:/usr/local/bin` 遗漏系统网络工具目录。拓扑/协议尚未运行，不能解释为撤销失败。系统 `/usr/sbin/ifconfig` 已确认存在；MiniNDN root PATH 增加 `/usr/sbin:/sbin`，保留编译器绝对路径约束。原始 `.codex-tmp/merge-20260906/minindn-user-revocation-r1/output.log` 保留；下一轮使用新目录。

## 2026-09-06 — Full integration after static fences

`integration-static-r1` 仍运行时已发现两个首边界：`Spec175NativeTinyOnnxExpiredDeadlineCleansProviderState` 的 provider failure count 不符，以及 `ProductionNativeHandlersRunD2h212ToCompleteOracleResponse` 缺一个角色/最终 oracle。保留本次完整 GDB 日志，先对照新增排队 fence 与原始 deadline/cleanup 状态更新，不能直接放宽 timeout 或删除拒绝断言。定向生命周期、证书撤销及 Controller45项通过不能替代该全模块结果。

## 2026-09-06 — Static repair cross-review lifetime finding

Build static-review R1 主动 SIGINT（exit68，154.920 s）。交叉复审发现新增 handler `!current()` 分支在 Provider 析构排空队列时仍向 Face 投递裸 `this` 的失败回调；析构后 dispatch 存在 UAF。停止当前构建，先在投递前和回调内检查共享 stopping token，并补 queued-handler 析构回归；不得把中断视为构建通过。原始 `.codex-tmp/merge-20260906/build-static-review-r1/` 保留。

## 2026-09-06 — Static review and full-module recount

NFD 定向 R1 进一步定位：提供私有 NFD 后，首边界变为 PUBPARAMS readiness timeout（10.418 s），因为 Controller 主 Face 是 DummyClientFace，不能与独立真实 probe Face 经 NFD 往返。不是仅缺守护进程。12处 policy fixture 启动改为已有 test-access 调用真实 `registerInterestHandlers()`，不设置 ready、不替换策略/签名处理；完整 `start()` 仍由13项 standalone readiness 和真实 NFD 测试覆盖。原始 `.codex-tmp/merge-20260906/integration-nfd-preflight-r1/`，NFD `/tmp/ndnsf-int-ntroyrk7/`，owned child cleanup=0。

Python import isolation R1 **43 passed / 3 skipped**（12.630 s），CLI loader 恢复 `sys.path` 后与 YOLO 联合执行通过；包含路径保持的新回归。

完整 module 日志纠正先前只统计部分 suite 的摘要：integration R3 为 **140/154 PASS、14 failed**，R4 为 **142/154 PASS、12 aborted**。其中 11 项是 Controller fixture 的 Face 尝试连接不存在的私有 NFD socket；另 1 项 certificate revocation 的 Controller 大写 digest 与 User/Provider 小写 digest 不匹配，不能归类为时间波动。原始目录 `integration-r3`、`integration-r4` 保留。MiniNDN 前先修复并重新验证。

当前 Python R1：2158 passed / 8 failed / 22 skipped；八项 YOLO 导入失败，独立 YOLO 19 项通过，正在检查联合执行时的 lazy import 首异常。原始 `.codex-tmp/merge-20260906/python-current-r1/output.log` 保留。Context Mode 的 `session-events` timeline 恢复查询被 guard 拒绝，继续以仓库、原始日志为准。

静态审查发现 User identity prefix retry 捕获局部 `onFail` 引用，构造结束后的失败回调存在 use-after-free；修复后须覆盖延迟重试。

## 2026-09-06 — D2h predecessor boundary / integration R3

定向 R2 使用了 Boost.Test 不接受的逗号连接完整路径，exit 200，未执行协议用例；原始 `d2h-regression-r2` 保留，改用同一 suite 下的 `ProductionNativeHandlersRunD2h*` selector 重试。

完整 R3：90/92 PASS，exit 201，183.621 s，无崩溃。Trace R1 首次失败为 `NDNSF_DATA_V1 HMAC verification failed`：compact segment 的认证预算原由 capability 生成，接收方错误地用可更严格的 Selection edge deadline 恢复 AAD。恢复 capability 的原始认证字段，同时保留 edge 对实际取数的 deadline 限制；D2h 121/212 既有测试正好覆盖两者不同的情况。

R3 已通过原崩溃的 targeted-stream 阶段；D2h 121/212 仍分别只观察到第一阶段 1/2 个角色，未得到完整 oracle response。按后继准入、依赖取数、scope-key 解密顺序使用独立 trace 定位；不扩大超时或删除 oracle 断言。原始 `.codex-tmp/merge-20260906/integration-r3/output.log`。

## 2026-09-06 — Owner wheel closure

R2 的临时环境来源断言仍失败，说明共享第三方 site 的环境不能依赖 pip 默认同版本判定。安装使用 `--ignore-installed` 强制这组本地产物进入临时 venv，并保留具体越界模块路径诊断；不卸载或替换主机包。

全量 Python 首次缺 `conversation.py`；补 SDK wheel 所有权后 wheel-closure R1 进入已声明 cryptography 依赖缺失边界。安装测试原来 `--no-deps` 且空 venv；改为离线复用测试主机第三方依赖，同时强制每个已导入 DI 模块来自临时 venv，保留 wheel 文件不碰撞与卸载后不可导入的断言。原始 `.codex-tmp/merge-20260906/wheel-closure-r1/output.log`。

## 2026-09-06 — Full Python diagnostic R2

后续 current-fixes R1（7 failed / 123 passed）与 R2（5 failed / 53 passed）首边界已定位为旧 fixture 和本机新导出模型与远端固定 registry 的身份差异；显式本地临时 registry 保留严格 hash 校验，修复依据见 [resolution design](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#resolution-design)。

3079 passed / 128 failed / 39 skipped，exit 1；旧实验目录/冻结基线缺失、显式 native binary/model 输入未设置，以及当前 facade、clock、build-prefix 测试夹具漂移。按既有 `run_spec175_python_gate.py` 的历史诊断与当前兼容性门分工，保留全量失败，不改旧 hash，不重跑历史 SIF/Tiger；当前合并覆盖的 Core/Repo/UAV/DI 测试另行明确选择并修复。原始 `.codex-tmp/merge-20260906/python-r2/output.log`。

## 2026-09-06 — Provider detached fetch lifetime / integration R2

GDB 捕获旧 Provider assignment worker 在销毁后调用 `Face::getIoContext()`；不是当前 targeted-stream 用例的独立失败。改为 Provider 所有的有界 fetch pool，关闭时取消等待、join，排队回调先检查共享关闭标志。原始 `.codex-tmp/merge-20260906/integration-r2/output.log`，完整记录见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#resolution-design)。

## 2026-09-07 — Spec182 T006-C: worker digest gate rejects every valid sha256 digest

- **Area**: spec182 T006-C Bounded Native Worker
- **Symptom**: Spec182OnnxWorkerProtocol suite — 16 failures across
  `MetadataAcceptsCanonicalEnvelopeRoundtrip`,
  `MetadataRejectsDigestFormatAndPayloadMismatch`, and
  `SubprocessUnregisteredThenRegisteredMatchesInProcess`. The roundtrip case
  showed `check.ok` false with `failureCode = DI_NATIVE_ONNX_WORKER_METADATA`,
  failureMessage `request metadata recipeDigest is invalid`; the child worker
  rejected the same envelope that the in-process validate call produced.
- **Root cause**: `isSha256Digest()` in
  `NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.cpp`
  required `value.size() == 66` and only checked 7+64 = 71 bytes would be
  valid; every canonical digest (`sha256:` prefix + 64 hex) is 71 bytes, so
  the format gate rejected every digest before the payload-digest compare
  could ever run, and the parsed certified slice was never populated.
  Symptom cluster was one root cause: (a) envelope gate METADATA instead of
  ok; (b) digest-payload mismatch classified METADATA instead of RECIPE;
  (c) the real worker child (stale binary, pre-fix digest gate) rejecting
  the envelope with a METADATA error frame.
- **Fix**: size gate corrected to 71 with a comment
  (`"sha256:" (7) + 64 hex`). Diagnostic staging was added and later removed
  from the failing test; after the fix all 21 Spec182OnnxWorkerProtocol cases
  pass against a rebuilt worker binary.
- **Ref**: re-run command
  `./build-nac182/unit-tests --run_test=Spec182OnnxWorkerProtocol
  --log_level=test_suite`; run dirs retained under the suite output.
- **Lesson**: a format gate with a wrong length constant fails every valid
  input silently as "invalid", so an always-rejecting validator can look
  like a roundtrip/envelope bug; assert length against `prefix + N hex`
  rather than a magic total.

## 2026-09-07 — Spec182 T006-C: spec181-assembly-parity fails full rebuild on missing onnxruntime include

- **Area**: spec182 T006-C Bounded Native Worker (full-build regression path)
- **Symptom**: the first full `./waf build` after editing tests/wscript
  recompiled all 635 tasks; `spec181-assembly-parity` (tests/wscript) failed
  compiling `NativeOnnxRecipeAssembler.cpp` with
  `onnxruntime_cxx_api.h: No such file or directory`, because its `use=`
  closure was `... ONNX ...` without `ONNXRUNTIME` while its compile line
  carried `-DNDNSF_DI_ENABLE_ONNXRUNTIME_CPP` and the ONNX prefix include
  (`repo/.codex-tmp/spec182-t001-dependencies/onnx-install/include`) only
  ships ONNX 1.17, not the runtime headers (`/opt/onnxruntime/include`).
- **Root cause**: the spec181-era target predates the spec182 unified
  runtime closure; every sibling DI target
  (`spec181-protected-runtime-closure`, `spec182-installed-consumer`,
  unit-tests) already lists `ONNXRUNTIME`. The target had not rebuilt since
  the runtime include became mandatory, so the failure surfaced only when a
  wscript change forced a full re-signature.
- **Fix**: add `ONNXRUNTIME` to the `spec181-assembly-parity` `use=` string
  in tests/wscript (closure drift, no behavior change). Full build green in
  11m51s at `-j2`.
- **Ref**: re-run command `./waf -o build-nac182 build -j2`.
- **Lesson**: waf content signatures mean an obsolete target stays green
  until any wscript change forces a full re-signature; after configuring on
  the unified dependency closure, audit remaining targets that compile
  adapter sources without `ONNXRUNTIME`.

## 2026-09-06 — Python collection and generation fixture R2

Python collection 缺 Repo binding 和三个既有辅助源脚本；integration 多 Provider generation fixture 的新 input endpoint digest 与旧常量冲突。分别补构建闭合/输入脚本和独立 endpoint identity，保留首边界证据。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#resolution-design)。

## 2026-09-06 — Real-NFD readiness collector R1

实际两轮启动/独立 Face round trip 已发生；旧 collector 要求 challenge Data 名后还有 `/`，与新 exact reply 不符，exit 1。修正 exact token 匹配并重跑，不以 collector failure 推断协议结果。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Controller readiness / versioned NAC parameters

Readiness R1 在 PUBPARAMS 首边界超时，exit 124。旧随机后缀探针与新 NAC 固定 generation 名称不兼容；分离 Authority fresh challenge 和当前版本参数验证，不回退 NAC 的版本真实性约束。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#resolution-design)。

## 2026-09-06 — Integration fixture build R4

D2a fixture 修订时保留了该 case 未定义的 `selectionObserved` 标志，编译拒绝；删除无关赋值后 R5 重建。Unit R2 已 751/751 PASS，不能替代 integration。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Native executable link and full integration R1

Native target 缺 RuntimeStatusStore source；全量 integration R1 60/92 PASS 后以 139 退出，包含 targeted stream memory fault 和旧 wrapper 导入。先补 target source、重建同源 binding 并用 GDB 定位崩溃。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Merged legacy ingress integration R1

完整 suite 中旧 ingress 流程报告空 assignment、错误 role 和 D2b 未完成；保留原始 R1 后定向检查 Selection 首边界。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Merged full unit R1

747/751 PASS，4 failed（2 aborted），exit 201。空 tensor overflow 检查除零、manifest size 和选中 Provider 加密输入流程失败；逐项定位，不把旧失败当作允许项。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Merged SVS catch-up arguments

NDNSF build R1 捕获混合调用：catch-up 方法名已恢复，但自动合并保留旧方法的 bool 参数尾部。恢复调用方已有的数量和毫秒年龄实参；保持现有接收与权限语义。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#resolution-design)。

## 2026-09-06 — NAC installed pkg-config paths

首次 CMake 配置的 `.pc` 在 GNUInstallDirs 之前生成，include/lib 错指 prefix 根。显式 Waf prefix 不足以保证 Python 绑定使用新依赖；调整 NAC 初始化顺序并验证 fresh-config 导出。运行代码未变，完整 case 结果保留。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md)。

## 2026-09-06 — NAC AttributeAuthority test identity isolation

依赖 full test 41/46 PASS；五项 fixture 默认 Face 读入主机旧 PIB/TPM，尚未测试授权行为即签名失败。显式传入已有内存 KeyChain，保留断言并重跑完整 suite。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Merged Context Mode fixture contract

合并 guard 后 42 PASS / 3 FAIL，首次边界为旧 Claude fixture 的 platform/registry 配置；更新有效 fixture，保留缺 hook/缺 flag 的失败断言。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md)。

## 2026-09-06 — Integration dependency preflight R2

Boost 路径修复后，NAC test link 仍调用 Linuxbrew ld，系统 OpenSSL/dl 符号解析失败。固定系统工具链再构建；原始 R2 日志和首边界见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#build-preflight-r2)。

## 2026-09-06 — Integration dependency preflight R1

NAC-ABE test link 选中缺失的 local Boost 1.82 库；NDNSF configure 拒绝尚未安装的目标 NAC prefix。均为构建前置失败，不是协议结果。先固定系统 Boost 并完成依赖测试安装，再重跑 NDNSF configure。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#build-preflight-r1)。

This is the repository-level index for failed, blocked, and `UNQUALIFIED`
attempts. It is an engineering memory, not a replacement for the active Spec,
the source tree, or a raw run directory.

## Read-first rule

At the start of every substantial task, read the newest entry in this file and
open its durable evidence record. If the entry names a raw log under the
ignored workspace temporary directory, inspect that log with targeted
`rg`/`tail` queries before choosing the next command. Then read the applicable
documents in
[`architecture-reading-guide.md`](architecture-reading-guide.md).

A failed preflight, startup barrier, or evidence collector is not a protocol
result. Do not retry a later gate, reuse a candidate, or claim a PASS until the
failure's controlling boundary and invalidation effect are understood.

## Current failure index

**Experimental consolidation closure (2026-09-06): development checks PASS.**
原生合并 `c770f18b` 的unit759/759、integration154/154、current Python
2171 passed/22 skipped和三个MiniNDN场景均PASS；新目录关联工具162 passed/
3 skipped，新Tiger工具58/58 PASS。下方collector RED由精确证据核对和进程组
清理修复关闭；原始失败保留。Tiger Local R8和B003运行验收仍未关闭，用户已暂停实验。
见 [integration closure](../specs/182-native-di-python-bindings/evidence/integration-20260906.md)
及 [Tiger baseline](../Experiments/TigerCluster/docs/two-node-baseline.md)。

**Tiger baseline collector review R1 (2026-09-06): expected regression RED.**
新增ACK/provider/request/selection与wrong-root边界证据负例在旧collector上
19 failed / 13 passed / 20 deselected，0.23s，证明它会接受不完整或矛盾证据。
这不是网络结果；保留 `.codex-tmp/merge-20260906/tiger-baseline-collector-red-r1/`。
修复后相关unit必须通过，B003实际运行仍未完成；见
[baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md)。

**Tiger two-node baseline Local R8 (2026-09-06): wrong-root boundary mismatch.**
All normal service, permission-rejection and cleanup checks passed. The wrong-root
child rejected PUBPARAMS authentication with abort134 before permission delivery.
Preserve this FAIL; classify only this exact isolated authentication abort in the
next run, rejecting unrelated crashes/timeouts. See the
[baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R7 (2026-09-06): observer/lifecycle FAIL.**
The service returned correct ECHO, but generic V2 binding leaves authentication
metadata unset. Replaced the unavailable-field assertion with observed native
ACK/Selection plus an actual wrong-root rejection obligation. The old Controller
wrapper cannot join its infinite native loop; use the existing C++ executable.
See the [baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R6 (2026-09-06): permission bootstrap FAIL.**
Raw signed roundtrips, signature negatives and PUBPARAMS succeeded. Controller
lacked public target certificates in its PIB and refused permission encryption;
added public-only imports with ndn-cxx readback and unchanged private-key checks.
Also isolated session state and corrected TERM ordering for FUSE-backed containers.
See the [baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R5 (2026-09-06): pre-NFD import FAIL.**
The image exposes UnixFace in stream_socket, not stream_face. Runtime preflight
stopped both workers before NFD startup; corrected the actual module path.
See the [baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R4 (2026-09-06): probe import FAIL.**
Multiline NFD/route configuration succeeded on both local instances. The probe
used KeychainSqlite instead of the image's KeychainSqlite3; corrected the symbol
and moved full application imports into pre-NFD inspection. See the
[baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R3 (2026-09-06): NFD management startup FAIL.**
Both NFDs aborted during internal FIB registration (10021). Review found compact
INFO list entries could not preserve privilege/policy nodes; restored multiline
INFO generation before retry. No protocol result; owned processes reaped. See
the [baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R2 (2026-09-06): identity setup FAIL.**
ndnsec refused root certificate installation into a nonexistent role-local root
identity. The validator already loads the public root file; removed the redundant
PIB installation. No NFD started; private state was cleaned. See the
[baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R1 (2026-09-06): host preflight FAIL.**
The local Python lacks str.removeprefix; version parsing stopped before identity
or NFD startup. Replaced it with an explicit prefix check and slice. Raw output
and subsequent attempts are indexed in the [baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger review sync R1 (2026-09-06): 58/58 tool checks PASS.**
Adopted the reviewed supervisor fixture expectation for the existing
collector-before-terminal-validation order, retaining FAILED/cleanup checks,
and restored sys.path after profile-test import. The 51 prior checks plus
7 collector positive/negative checks pass; the migration R1 assertion failure
is closed. No production runtime or cluster was run. See [review sync evidence](../specs/182-native-di-python-bindings/evidence/tiger-directory-migration-20260906.md#review-sync-r1).

**Tiger directory migration unit R1 (2026-09-06): 50 PASS / 1 FAIL.**
The supervisor no-result unit expects TERMINAL_RESULT_MISSING but receives
CollectionError from the collector that now runs first. All 64 moved files
retain their original bytes and modes. The same named test with identical
bytes in a separate physical pre-migration layout reproduces the same failure;
this remains a baseline tool/test issue, not a relocation regression.
No SIF or Tiger execution occurred. See [migration evidence](../specs/182-native-di-python-bindings/evidence/tiger-directory-migration-20260906.md).

**Spec181 delivery-tool counterfactual R1 (2026-09-06): expected semantic RED.**
The 40-check baseline passes. Removing only the exit-code rejection makes the
same named regression fail with DID NOT RAISE, proving it detects false PASS
despite a nonzero child exit. Preserve the mutant and restore the production
check before final validation. Real T008 qualification and sealing remain open.
See [delivery tool evidence](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t009-delivery-tool-20260906.md).

**Spec181 T008 exporter adoption R1 (2026-09-06): focused PASS.**
The isolated exporter/adapter/numerical run passes 41 checks with no skips.
Explicit checkpoint input, actual 32/640 ONNX export, registered signatures,
640 CPU ORT versus PyTorch oracle, and production User negative branches are
covered. Preserve 22 fixed-shape export warnings; candidate local-delivery
tool closure and complete qualification remain open. See
[exporter adoption](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#exporter-adoption-r1).

**Spec181 T008 input closure R3 (2026-09-06): focused PASS.**
The isolated inventory/supervisor run passes 92 checks after the actual RED.
Checkpoint files and registry-referenced public keys are bound; changed inputs
are rejected before children. The shared wrapper output-source dependency is
included with explicit CLI precedence and missing-input rejection. Three
registered Ed25519 public keys pass digest/identity checks. Full qualification
remains open. See [R3 closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#input-identity-closure-r3-and-public-material).

**Spec181 T008 input identity R2 (2026-09-06): missing shared CLI dependency.**
All new input checks pass; the isolated two-file run has 89 PASS and one FAIL.
The existing streamed-generation wrapper regression exposes an unadopted
runner-owned output-directory option. Preserve its CLI failure, adopt only
the output-source/default rejection behavior, and retain the existing test.
See [R2 boundary](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#input-identity-closure-r2-and-shared-wrapper-dependency).

**Spec181 T008 input identity R1 (2026-09-06): semantic RED.**
Fourteen focused checks expose unbound checkpoint and registry public-key
files; five existing drift checks pass. The actual gate attempts its child
entry after either new input changes, caught before a real child launch.
Repair the inventory input owner; no full suite or network case ran. See
[input identity R1](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#input-identity-closure-r1).

**Spec181 T008 assembly closure R4/R1 (2026-09-06): focused PASS.**
The corrected shared-source registration builds both complete C++ test targets.
Only the three named assembly cases ran: 3/3 cases and 138/138 assertions pass
through actual CPU ORT load/warmup with bound Provider/model/plan identities.
The complete suite remains blocked on remaining source/input closure. See
[assembly closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#assembly-test-build-and-focused-closure).

**Spec181 T008 full test build R3 (2026-09-06): source-registration link failure.**
The migrated assembly test compiles, but its shared preparation implementation
was added to grant_sources instead of di_integration_sources. Move that
registration; no suite ran. See [assembly migration](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#assembly-test-api-migration-plan).

**Spec181 T008 full test build R2 (2026-09-06): stale assembly-test API.**
The ProtectedRuntime migration compiles; integration compilation now stops at
ndnsf-di-native-assembly.t.cpp:341, which calls unavailable runtimeMetricsSnapshot().
No complete suite ran. Preserve real ORT load/execution proof while migrating
the check to the current runner contract. See [full build R2](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#full-test-build-r2).

**Spec181 T008 adoption C R1 (2026-09-06): unadopted GPU-source dependency.**
Fifteen checks stop at fixture compilation on the absent CudaDeviceIdentity header; one
source assertion exposes the old GPU metadata path. Inspection confirms this
draft targets uncommitted CUDA/profile changes. Preserve it for T010/T011
preparation, retain CPU-only local claims, and carry the concrete GPU evidence
gap into delivery. No production source was changed. See [Batch C](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-c).

**Spec181 T008 test adoption B R2 (2026-09-06): focused PASS.**
All 19 application/Merge checks pass without skips using explicit signed
canonical inputs. The stale timeout assertion follows the existing request
budget contract; no production deadline changed. Five local test/tool
dependencies remain. See [Batch B](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-b).

**Spec181 T008 test adoption B R1 (2026-09-06): stale source assertion.**
Eighteen checks pass; one expects a historical hard-coded User no-progress
timeout. Inspect its current parameter source before migrating the assertion.
No network attempt occurred. See [Batch B](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-b).

**Spec181 T008 runtime test migration R2 (2026-09-06): focused PASS.**
The final two-file target builds and passes 30 cases / 225 assertions.
Real verified grants now cover publish/fetch rejection and host/device lease
cleanup; credential-free checks remain fail-closed. Full-suite build and
source closure remain open. See [runtime test migration](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#full-test-build-r1-and-runtime-test-migration).

**Spec181 T008 full test build R1 (2026-09-06): stale test API.**
Compilation stops at the old ProtectedRuntime revoke/revoked calls; no full
suite executed. Keep revocation transferred, migrate current fail-closed
tests, and retain dataflow/zeroization assertions using real BoundGrantFixture.
See [runtime test migration](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#full-test-build-r1-and-runtime-test-migration).

**Spec181 T008 test adoption R3 (2026-09-06): focused PASS.**
The final four-file projection passes all 32 checks without skips after
removing the rejected temporary-path fallback. Source/test bytes match the
isolated projection; the complete C++ test-target build continues separately.
See [test adoption](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-a).

**Spec181 T008 adoption checkpoint (2026-09-06): commit hook blocked.**
The staged role-assembly test retained a development-temporary-path fallback.
No commit was created. Remove that fallback, keep explicit input validation,
and rerun the focused checks before committing. See [test adoption](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-a).

**Spec181 T008 test adoption R2 (2026-09-06): focused PASS.**
All 32 checks pass without skips using explicit canonical-package/registry
inputs. The four adopted files cover ACK provenance, truthful negative
verdicts, certified role assembly, and input/terminal ownership. Fifteen
other draft-test dependencies remain to review before complete qualification.
See [test adoption](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-a).

**Spec181 T008 test adoption R1 (2026-09-06): 31 PASS, one input failure.**
The role-assembly regression's hard-coded isolated registry lacks its
catalogue-authority public-key file. Failure precedes assembly at signature
preflight. Bind the test to the explicit package and registry already used by
R19, retaining signature verification. See [test adoption](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-a).

**Spec181 T008 case-configuration R2 (2026-09-06): focused PASS.**
All 76 inventory/supervisor checks pass, including actual per-case child
configuration and pre-launch input-drift rejection. The complete T008 gate
still needs test-source/build closure and final case inputs; R19 remains the
completed T005 subject. See [R2 review](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#focused-configuration-r2-and-review).

**Spec181 T008 case-configuration R1 (2026-09-06): focused RED.**
Eleven assertions expose missing declaration validation, unbound case-policy
bytes, and absent per-case child configuration; four existing input-drift
checks pass. The repair stays in the local inventory/supervisor boundary.
See [configuration preflight](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#focused-configuration-r1).

**Spec181 formal Y-N R19 (2026-09-06): PASS; T008 preflight remains open.**
The maintained CLI completes all seven subcases, including three real grant
variants, on ce6a4ba0. All 63 application child exits are collected; source and
input identities remain unchanged, and no recorded PID or NFD remains.
See [R19 qualification](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-y-n-matrix-current.md#current-r19-qualification).
The next complete local gate needs case-specific configuration binding and
test-source closure; see [T008 preflight](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md).

**Spec181 exact-wire native R1 (2026-09-06): PASS.**
Committed ce6a4ba0 passes maintained native build and independent verify.
Both focused regressions also pass against the refreshed Core (21 and 270
assertions). The affected 12-dimension convergence review restores A05 PASS;
proceed to a fresh R19 matrix, retaining all earlier failures. See
[native review](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#native-identity-r1-and-convergence-review).

**Spec181 exact-tensor R6 (2026-09-06): focused PASS.**
Nine real DI/Provider/IMS cases pass 270 assertions: 1.4 MB compact transfer
fits signed packets (maximum 8,477 bytes), legacy format reconstructs, and
signature/commitment/context/bounds plus authenticated inner HMAC/index/legacy
binding mutations reject at their named boundaries. Commit the shared repair,
refresh full native identity and re-audit before a new formal matrix. See
[wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-repair-r5-and-authenticated-inner-rejections-r6).

**Spec181 exact-tensor R4 (2026-09-06): legacy fixture exceeds packet limit.**
Large compact transfer and four real verifier rejections pass (5/6 cases).
The legacy fixture's 7,000-byte payload segment plus old metadata reaches
10,555 signed bytes; Core correctly rejects before decoding. Use small legacy
segments to test compatibility, retaining the unchanged 1.4 MB compact case.
See [wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-repair-r3-and-negative-probe-r4).

**Spec181 exact-tensor R2 (2026-09-06): test bridge correction required.**
Compact production transfer reconstructs the 1,400,017-byte object and all
signed packets fit; 412/413 assertions pass. The sole failure counts 404
packets versus 202 because both fixture peer bridges and manual bridges run.
Disconnect fixture peer bridges before custom forwarding; keep the same
packet-count, content and size assertions. See
[wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-probe-r2).

**Spec181 exact-tensor R1 (2026-09-06): semantic RED at publication.**
The real DI publishOutput of a 1,400,017-byte tensor creates a 17,546-byte
signed manifest and correctly fails the repaired Core limit. Build passed;
the failure is the remaining codec boundary. Apply compact exact encoding
with authenticated reconstruction and legacy compatibility before retry.
See [wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-regression-r1).

**Spec181 exact-wire Core R2 (2026-09-06): focused PASS.**
Initial inventory rendering separately rejected the new evidence file's missing
layer header; add the explicit scoped header before rerunning that document check.
The production Core rebuild and unchanged Provider/IMS regression pass all
21 assertions: 8,799/8,800-byte signed packets are readable; 8,801-byte packets
reject and a late batch size failure exposes no earlier item. DI compact
representation/consumer repair remains BLOCK before native refresh and matrix.
See [wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#core-repair-r2).

**Spec181 exact-wire Core R1 (2026-09-06): semantic RED reproduced.**
The real Provider/IMS pull regression passes 18/21 assertions but accepts an
8,801-byte signed Data and leaves an earlier batch item readable after a late
oversize item. Adopt full signed-wire prevalidation for the whole batch, then
repeat the unchanged test in a fresh run. See
[wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#core-regression-r1).

**Spec181 R18 diagnosis (2026-09-06): BLOCK at NDN Data wire size.**
The first actual send boundary is now identified: BackboneNeck's exact
MANIFEST Data encodes to 19,658 / 10,883 bytes and a SEG Data to 14,191,
above ndn-cxx's 8,800-byte limit (169 event-loop exceptions). The exact
NDNSF-DI filter exists; downstream deadlines are consequences. Review the
existing compact transport changes and pre-publication size guard as a
bounded shared unit, retaining signature/content commitments. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#exact-dependency-boundary-r18).

**Spec181 T005 formal R18 (2026-09-06): BLOCK at exact dependency transfer.**
BackboneNeck now executes with actual CPU ONNX evidence and completes its
role. DetectShard0/1 cannot fetch its exact tensor manifests; Merge then
times out on their outputs. Inspect publication, cache response and routing
before another run. Source/input identities remain unchanged and NFDs exit.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#exact-dependency-boundary-r18).

**Spec181 backend native refresh R1 (2026-09-06): PASS.**
Committed 214df1d6 completes maintained native build and independent verify;
both report native identity OK. The tested registration change and retained
device/error contracts pass affected convergence review. Resume a new formal
matrix; focused CPU checks do not establish matrix qualification. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-native-identity-r1).

**Spec181 backend repair R4 (2026-09-06): focused PASS.**
The maintained native Provider rebuilds (35.522 s); five real executable
checks pass (1.02 s). Legacy/public CPU names load and warm a real ONNX
model; unknown names and invalid execution-provider metadata still reject.
Commit only registration blocks and tests, then refresh native identity and
re-audit before the next matrix. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-registration-repair-r4).

**Spec181 backend probe R3 (2026-09-06): semantic RED reproduced.**
With corrected evidence assertions, legacy CPU load/warmup and unknown-backend
rejection pass; three public-name checks fail at missing registration. Adopt
only the two registration blocks, rebuild the maintained Provider and repeat
the real executable checks. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-registration-regression-r3).

**Spec181 backend probe R2 (2026-09-06): registration RED and assertion correction.**
Three checks reach missing public backend registration. The legacy backend
actually loads/warms the model, but its test misreads existing string-valued
evidence and nested device fields. Unknown-backend rejection passes. Correct
the schema assertion before the next focused run. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-registration-probe-r2).

**Spec181 backend probe R1 (2026-09-06): BLOCK at test collection.**
An extra parenthesis in the new test prevents collection (0.36 s); no native
process ran. Correct the test syntax and retain R1 before a fresh R2 probe.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-registration-probe-r1).

**Spec181 T005 formal R17 (2026-09-06): BLOCK at backend registration.**
All four Providers pass external assignment validation and enter their handler.
BackboneNeck then fails with no NativeModelRunner backend registered:
onnxruntime-cpu; dependent-role fetch deadlines follow. Preserve that first
boundary, repair actual backend registration and re-audit before retry. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#native-backend-boundary-r17).

**Spec181 digest native rebuild R1 (2026-09-06): PASS.**
Committed e6f44b65 rebuilds the Core library, native Provider and Python
extension; maintained build and independent verify both report native identity
OK. Source/byte checks and exact assignment rejection remain intact. Affected
convergence review PASS; proceed to a fresh formal matrix. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-digest-native-rebuild-r1).

**Spec181 digest repair R3 (2026-09-06): focused PASS; native rebuild pending.**
Four actual C++ helper checks pass (1.73 s) after the isolated Provider emits
canonical lowercase hex. Exact assignment size/digest checks remain intact.
Commit this unit, rebuild the native runtime and re-audit before a new matrix.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-digest-repair-r3).

**Spec181 digest probe R2 (2026-09-06): RED reproduced.**
All four actual-helper comparisons fail only at Provider uppercase hex;
User matches independent SHA-256. Normalize Provider output to the existing
canonical lowercase contract, preserving exact byte/size checks. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-digest-regression-r2).

**Spec181 digest probe R1 (2026-09-06): BLOCK at linker selection.**
The focused helper probe fails at compilation/linking (4 setup errors,
1.60 s), before digest comparison: system g++ selects Homebrew ld from PATH.
Use the system toolchain explicitly and retain the original log. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-digest-probe-r1).

**Spec181 T005 diagnostic R16 (2026-09-06): BLOCK at external assignment validation.**
Core INFO logging shows each Provider receives and queues Selection, then
fails assignment preparation with external collaboration assignment size or
digest mismatch. The duplicate request log is not the controlling boundary.
Inspect assignment publication, fetch and validation on the same source.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#external-assignment-boundary-r16).

**Spec181 T005 formal R15 (2026-09-06): UNQUALIFIED after Selection commit.**
The sealed-plan repair reaches four-role Selection commit, then the User
receives REMOTE_RESPONSE_FAILED. Provider WARN logs contain duplicate request
rejections but no native execution failure reason; this does not yet identify
the controlling cause. Inspect the Core request/Selection boundary and obtain
bounded diagnostic logs before changing behavior. Source/input identities
stay unchanged and all NFDs exit. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-response-boundary-r15).

**Spec181 sealed-plan repair R3 (2026-09-06): focused PASS.**
All 22 sealing/candidate checks (0.97 s) and 36 existing plan integration
checks (0.77 s) pass in the isolated source. Fetch references remain separate
from canonical identity, immutable and digest-bound; malformed values reject
before commit, while legacy/local-preparation defaults remain compatible.
Affected A05 review PASS; resume a new formal matrix after committing. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sealed-plan-repair-r3).

**Spec181 sealed-plan validation R2 (2026-09-06): BLOCK at reference validation.**
The projected field resolves sealing; five normal/legacy/coverage checks pass.
Five malformed reference checks fail because non-string false values and
control characters are accepted. Require strings and reject control chars,
retaining the deliberate empty local-preparation reference. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sealed-plan-validation-r2).

**Spec181 sealed-plan regression R1 (2026-09-06): RED reproduced.**
Ten focused checks reproduce the missing field at the actual V3 sealing
expression (1.72 s). Project the existing shared contract and validate
transport forwarding, digest binding, legacy defaults and invalid references.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sealed-plan-regression-r1).

**Spec181 T005 formal R14 (2026-09-06): BLOCK at sealed-plan reference closure.**
The shared assembly repair permits request planning to reach plan sealing.
The committed SealedCollaborationPlan lacks artifact_fetch_data_names,
already consumed by placement. Review/adopt the existing field and digest
binding, verify plan production/consumption, then re-audit. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sealed-plan-boundary-r14).

**Spec181 canonical binding/assembly repair R6 (2026-09-06): focused PASS.**
The isolated Waf parity target builds successfully; all 40 canonical,
candidate and actual C++/Python assembly checks pass (12.91 s). Actual YOLO
two-candidate binding, recipe and publication-port checks pass as well.
Affected convergence review PASS. Commit only the tested shared CPU unit,
then resume a fresh formal matrix. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-and-assembly-repair-r6).

**Spec181 canonical assembly R5 (2026-09-06): BLOCK at focused harness inputs.**
Thirty-two Python checks pass. Eight native checks lack the required parity
binary; the extended recipe probe incorrectly includes the non-ONNX Merge
role in its graph assertion. Correct those focused harness inputs before
further validation; no network attempt was made. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-assembly-boundary-r5).

**Spec181 canonical recipe R4 (2026-09-06): BLOCK after binding repair.**
R3 actual binding/publication and 24 focused checks pass. Extending the probe
through real role certification reveals that the committed recipe rejects
COMPONENT_SET zero intervals. Add the reviewed component/external-initializer
assembly changes to this source unit; retain unrelated CUDA provider selection
outside the unit. See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-repair-r3-and-recipe-boundary-r4).

**Spec181 canonical-binding regression R2 (2026-09-06): RED reproduced.**
With the complete Python path and private offline NDN environment, actual
YOLO describe reproduces the missing canonical_graph_digest TypeError.
Proceed with the reviewed shared dependency unit and focused checks. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-regression-r2).

**Spec181 canonical-binding regression R1 (2026-09-06): BLOCK at probe import.**
The isolated probe lacks the Repo Python path and stops at SDK import before
the binding constructor. Complete the explicit Python/private NDN environment
for the next focused run. The source/reference contract,
shared deployment consumer and their tests form the reviewed dependency unit.
Validate that unit plus actual two-candidate publication before re-auditing.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-regression-r1).

**Spec181 T005 formal R13 (2026-09-06): BLOCK at canonical artifact binding closure.**
The epoch repair permits the User to request a V3 task. YOLO describe passes
canonical_graph_digest to the committed CanonicalArtifactBinding, whose
dataclass lacks that field. The working tree contains the shared canonical
reference extension, while the isolated commit omits it. Review and validate
the complete binding/publication dependency before another run; all NFDs
exited and source/input identities stayed unchanged. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-boundary-r13).

**Spec181 subcase-epoch repair R2 (2026-09-06): focused PASS.**
All 117 runner/matrix/grant seam checks pass (3.99 s). Child epoch now matches
the publication/Provider runtime inputs; the protected Y-B and all three
Y-N-E mutations retain their grant configuration, and the parent environment
is unchanged. Affected convergence review PASS; T005 needs a new formal run.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#subcase-epoch-repair-r2).

**Spec181 subcase-epoch regression R1 (2026-09-06): RED reproduced.**
Seven plaintext cases inherit the protected matrix epoch; four protected
profiles pass. The focused production runner capture stops before network
startup (7 failed / 4 passed / 88 deselected, 2.06 s). Bind child epoch to the
same runtime inputs used by publication and Provider process specifications.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#subcase-epoch-regression-r1).

**Spec181 T005 formal R12 (2026-09-06): BLOCK at subcase epoch environment.**
Committed fixture loading passes. Y-N-O User inherits the matrix's protected
epoch, although runtime publication and process specifications choose the
plaintext control epoch; its grant seam then raises SPEC181_REQUESTER_PRIVATE_KEY
KeyError. Scope the child epoch to the selected subcase and regression-test
both plaintext controls and protected Y-N-E before rerunning. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#subcase-epoch-boundary-r12).

**Spec181 fixed-input closure repair (2026-09-06): focused PASS.**
The existing PPM and provenance README are adopted without byte changes.
All 22 numerical regression checks pass (7.97 s); the actual canonical
package reference loader verifies the fixture digest and shapes. A05 is
re-audited PASS; verify the committed fixture in the isolated checkout
before resuming the formal matrix. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#fixture-closure-repair).

**Spec181 T005 formal R11 (2026-09-06): BLOCK at fixed fixture source closure.**
The repaired lock permits Controller publication, Repo, and four native
Providers to become ready. User startup fails because the isolated commit
lacks tests/fixtures/spec180/yolo26n/fixed-fixture.ppm. The existing untracked
162-byte fixture matches the manifest digest. Reopen A05, adopt the fixture
and its provenance, validate the real reference loader, then re-audit before
the next matrix. Source/input identities stayed unchanged; NFDs exited. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#fixed-input-source-closure-r11).

**Spec181 T005 formal R10 (2026-09-06): BLOCK at Controller publication initialization.**
The explicit shell environment repaired process spawning. NFD readiness and
Controller startup pass, but the co-located publication ServiceUser constructor
throws `Failed to acquire file lock`. The maintained matrix exits 2 at Y-N-O;
The focused syscall probe finds EACCES before flock: the UID-0 lock path is
owned by UID 1000, with no kernel lock or fuser occupant. Preserve this stale
file in R10 before letting the runtime recreate it; no Core change is needed.
no protocol result is established. Source/input identities remain unchanged,
and all NFD processes exited. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#controller-publication-boundary-r10).

**Spec181 T005 formal R9 (2026-09-06): BLOCK at explicit shell environment.**
The new diagnostic identifies KeyError in Mininet node.py:419: shell=True reads
os.environ['SHELL'], absent from the launch environment. Preserve the exact
frame chain; set SHELL=/bin/bash explicitly for R10. No Controller was launched,
all NFDs exited, and source/input identities stayed unchanged. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-application-boundary-r9).

**Spec181 spawn diagnostic repair R3 (2026-09-06): focused PASS.**
All 93 runner/matrix checks pass. Partial-start cleanup and first-failure stop
remain intact; the new exclusive diagnostic records type and frame locations
without exception text or locals. The actual R8 spawn cause still requires a
new run. See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#spawn-diagnostic-repair-r3).

**Spec181 spawn diagnostic R2 (2026-09-06): BLOCK at test import.**
Two-file checks produce 1 failed / 92 passed (1.55 s). The runtime writes its
new diagnostic; the new assertion lacks the json import. Preserve R2, add the
test import, and rerun. This is a test-fixture failure, not a runtime result.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#spawn-diagnostic-regression-r2).

**Spec181 spawn diagnostic regression R1 (2026-09-06): RED reproduced.**
The existing partial-start cleanup test now verifies a durable error boundary;
it fails because process-start-failure.json is absent (1 failed / 87 deselected,
0.90 s). Keep cleanup and failure verdicts intact; record only exception type
and frame locations, preserving first evidence without messages or locals.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#spawn-diagnostic-regression-r1).

**Spec181 T005 formal R8 (2026-09-06): BLOCK at application spawn diagnostics.**
NFD readiness/routing/keychains pass. Controller log creation is followed by an
immediate spawn failure; the matrix preserves only CONTROL_NOT_PROVEN and drops
the underlying traceback boundary. Preserve R8 and add exception type plus
file/function/line frames, without exception text or locals, before another
diagnostic run. NFD cleanup was verified. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-application-boundary-r8).

**Spec181 T005 formal R7 (2026-09-06): BLOCK at NFD readiness.**
All five NFD sockets exist, but node nfdc checks fail; no application child
started. The launcher inherited offline probe NDN_CLIENT_* overrides pointing
to unused.sock instead of node client.conf. Preserve startup diagnostics and
logs. R8 will isolate the parent via private HOME, remove the global overrides,
and retain explicit Python dependency paths. NFD/native Provider cleanup was
verified. See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-network-boundary-r7).

**Spec181 T005 formal R6 (2026-09-06): BLOCK at Mininet executable readiness.**
The explicit PATH omitted sbin and Mininet could not find ifconfig (exit 1).
Preserve R6. Verify required network tools and append the system sbin paths for
R7 while preserving Python/native resolution order and recording the new launch
environment. This is startup readiness, not a protocol result. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r6).

**Spec181 T005 formal R5 (2026-09-06): BLOCK at launch environment parsing.**
The temporary parser rejected the registered SPEC180_CASE_OUTPUT_DIR in the
Y-N environment. No case/state or runner was created. Preserve R5; accept that
specific field and override it with R6's unique output. The five-role Y-N input
uses the same model; the protected epoch remains explicit for Y-N-E. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r5).

**Spec181 T005 formal R4 (2026-09-06): WAITING_EXTERNAL_INPUT at case roles.**
Envelope ownership now passes. The Y-B baseline configuration lacks Y-N's
required FullModel capability, so maintained validation exits 78 before network.
Preserve R4; inspect and use the existing Y-N-specific inputs for a new R5.
The registered role-set requirement remains unchanged. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r4).

**Spec181 T005 formal R3 (2026-09-06): WAITING_EXTERNAL_INPUT at key ownership.**
The repaired sudo source gate passes on 88e7a458. Maintained input validation
rejects the developer-owned envelope key for root execution (exit 78), before
network. Preserve R3 and provision the same bytes as a 0600 root-owned file in
R4's private state, retaining the original key untouched. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r3).

**Spec181 sudo source repair R2 (2026-09-06): PASS; formal R3 next.**
The gate preserves SUDO_UID only for root plus the actual selected checkout
owner. Real sudo positive/negative tests and existing local gate regressions
pass: 51 checks (8.68 s), with Git overrides still stripped. A05 is re-audited
PASS; T005 remains incomplete and will use a new run directory. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sudo-source-repair-r2).

**Spec181 sudo source regression R1 (2026-09-06): RED reproduced.**
Real sudo Git checks yield 1 failed / 1 passed: the legitimate owner is rejected,
while the wrong UID remains rejected. The test also supplies hostile GIT_DIR
and GIT_INDEX_FILE overrides. Preserve red.log and repair only the matching
sudo-owner identity in the sanitized environment. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sudo-source-regression-r1).

**Spec181 T005 formal R2 (2026-09-06): BLOCK at sanitized source Git.**
Outer Git now accepts the actual sudo user, but production _source_git drops
SUDO_UID again and fails before network. Reopen A05's sudo checkout boundary;
add a real sudo regression and retain the UID only when it matches the selected
checkout owner. All Git override variables remain stripped. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r2).

**Spec181 T005 formal R1 (2026-09-06): BLOCK at launcher Git ownership.**
The explicit environment dropped SUDO_UID; root Git rejects the user's checkout
before invoking the maintained runner. Preserve R1. Retain the actual sudo
caller UID in the explicit launch environment for R2; do not write global Git
exceptions or weaken the source gate. No network started. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r1).

**Spec181 T007 convergence (2026-09-06): PASS; T005 next.**
A05's source/configuration/input/build/application boundaries now map to their
focused regressions and final committed checks. A01–A12 are closed within their
documented scopes. The 12-principle audit permits same-source local validation;
it is not qualification or development-delivery PASS. Historical failures below
remain preserved. See [audit](../specs/181-ndnsf-di-protected-grant-qualification/audit.md#a05-closure-matrix).

**Spec181 committed source R13 (2026-09-06): R12 resolved.**
Repo build intermediates are preserved outside the checkout. The strict source
guard passes for 6b9bb51c, and the real application/native preflight plus four
application imports pass on that commit without network. Overall T007 audit
remains the next gate. See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#committed-source-and-runtime-r13).

**Spec181 committed source R12 (2026-09-06): BLOCK at build intermediates.**
The isolated checkout matches 6b9bb51c with a clean tracked/index tree. The
actual source gate rejects untracked Repo setup build/src/ArtifactManifest.o.
Preserve the terminal result and move the complete intermediate build directory
to R12 before retry, retaining runtime extension and strict source checks. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#committed-source-reconciliation-r12).

**Spec181 source closure R11 (2026-09-06): focused failures resolved.**
The same isolated selected source passes 45 existing checks and 12 new candidate
binding checks. Actual application/native preflight and four application imports
pass without network. R1–R10 remain below as historical first-boundary evidence.
Final committed-source reconciliation and T007 audit remain open. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#focused-closure-r11).

**Spec181 focused source closure R10 (2026-09-06): BLOCK at request contract.**
Actual application/native preflight passes without network. Six-file checks
yield 9 failed / 36 passed (1.38 s): missing DIRequestEnvelopeV2 input transport
fields and InferenceApplication task arguments. Preserve R10 and close the two
request endpoints before retry. See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-and-focused-r10).

**Spec181 application preflight R9 (2026-09-06): BLOCK at verifier implementation.**
Export alone is insufficient: ProviderOfferTrustVerifier itself is absent from
committed SDK provider.py. Preserve R9 and validate the implementation and its
existing signature/ACK tests as part of source closure. No network ran. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r9).

**Spec181 application preflight R8 (2026-09-06): BLOCK at public verifier export.**
Actual publication/process_specs/native guard pass. The post-guard import probe
then finds user.py requires ProviderOfferTrustVerifier missing from SDK exports.
Preserve R8 and add the existing verifier export; no network ran. Native guard
success alone does not prove application import closure. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r8).

**Spec181 application preflight R7 (2026-09-06): BLOCK at local launch helper.**
Runtime publication passes. Actual process_specs calls legacy python_cmd with
repo/py_dir, which the committed helper lacks. Close the matching local helper
parameterization, retaining default compatibility. Preserve R7; no network ran.
See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r7).

**Spec181 application preflight R6 (2026-09-06): BLOCK at catalogue conversion.**
SDK/Provider imports pass. Runtime publication requires the uncommitted
PreSplitCatalogSnapshot.from_mapping contract. Preserve R6 and close the
matching validation/serialization dependency before retry; no native guard or
network ran. See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r6).

**Spec181 application preflight R5 (2026-09-06): BLOCK at Repo reference contract.**
The selected ApplicationInput contract requires LargeDataReference, absent from
committed repo_reference.py. Existing Provider/client/facades already consume
this shared publication/reference owner. Preserve R5 and close that dependency;
no network ran. See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r5).

**Spec181 application preflight R4 (2026-09-06): BLOCK at shared input contract.**
Candidate construction now passes. SDK imports then fail because the committed
Provider requires MAX_INLINE_INPUT_BYTES absent from adapters.base; the
coordinator also requires InputTransportMode. Preserve R4 and close the shared
input contract/export dependency before retry. No native guard/network ran.
See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r4).

**Spec181 application preflight R3 (2026-09-06): BLOCK at shared candidate contract.**
Repo's same-source build and import pass. Production YOLO publication then
constructs SplitCandidate with selection_priority, absent from the committed
contract, and fails before native guard/network. Preserve R3; close the exact
shared contract dependency without mixing unrelated worktree changes. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r3).

**Spec181 application preflight R2 (2026-09-06): BLOCK at Repo Python extension.**
Adding four existing YOLO source files to the isolated checkout clears actual
catalogue verification. Policy generation then cannot import
py_repoclient._py_repoclient, which has not been built in that checkout.
No native guard/network started. Preserve R2; use Repo's maintained build
against the same source, not an unknown worktree binary. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r2).

**Spec181 application preflight R1 (2026-09-06): BLOCK at committed Python adapter import.**
The clean a51f87b3 native build passes and emits its new receipt. Actual runner
validate_inputs then fails to import build_yolo26n_adapter from adapters.yolo,
wrapped as CANONICAL_CATALOGUE_VERIFY_FAILED. No network or native guard was
started. Preserve R1 and repair the committed adapter package closure before
retry. See [local runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r1).

**Spec181 local input identity R3 (2026-09-06): BLOCK at new fixture import.**
After R2 66 PASS, new input tests yield 3 failed / 69 passed (7.68 s): three
tests reference json without importing it, before the identity owner runs.
Real-child drift/collection checks pass. Preserve R3, repair the fixture import,
then rerun. See [local input identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-input-identity-20260906.md#focused-r2-and-fixture-failure-r3).
R4 fixes the missing import: 72 PASS (8.40 s). Actual fixture children preserve
completed results and reject input drift before network cases or at final
aggregation. Both CLI discovery paths consume the explicit environment.
Configured input identity unit CLOSED; actual runtime audit remains T007 work.

**Spec181 local input identity R1 (2026-09-06): BLOCK at external input bytes.**
Model/map/referenced-key replacement and package additions reach the forbidden
child boundary after inventory creation (4 failed); launch configuration binds
path strings only. No qualification child ran. Preserve R1 and bind the actual
external inputs in the shared inventory/gate owner. See
[local input identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-input-identity-20260906.md).

**Spec181 explicit config root R1 (2026-09-06): BLOCK at protected launch selection.**
The runner overwrites explicit NDNSF_SPEC180_CONFIG_ROOT with the HOME default.
Absolute/relative overrides and missing-key rejection fail (3 failed / 1 passed,
0.81 s); fixture stops before native preflight/network. Preserve R1 and honor
the explicit root before child HOME changes. See
[explicit configuration root](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-explicit-config-root-20260906.md).
R2 honors the explicit root, resolves it before child HOME/cwd changes, and
rejects a missing explicit key even when the default key exists: 24 focused
checks PASS (0.85 s). Configuration selection unit CLOSED; T007 remains open.

**Spec181 Waf tool identity R3 (2026-09-06): BLOCK at CLI fixture PATH mismatch.**
1 failed / 79 passed (1.81 s): an old CLI linkage test builds with fixture PATH
then verifies with ambient PATH. The new Waf identity check correctly rejects
that mismatch first. Align the CLI fixture environment and retain its original
wrong-Core rejection assertion. R3 log and patch preserved in
[Waf tool identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-waf-tool-identity-20260906.md#cli-fixture-boundary-r3).
R4 repairs the CLI fixture: 80 PASS (1.75 s). Actual Waf directory selection
matches the new owner in both current and isolated checkouts (80 source/resource
files each). Waf source/selection unit CLOSED; runtime/input closure remains
T007 work. Existing native receipts require a maintained rebuild for the new field.

**Spec181 Waf tool identity R2 (2026-09-06): BLOCK at fixture executable identity.**
57 failed / 18 passed (2.77 s): the existing fake interpreter lacks its
executable bit, so real PATH resolution rejects it before mocked build; one
environment assertion also predates child-only WAFDIR. Preserve R2 and repair
fixtures before retry, without relaxing production resolution.
See [Waf tool identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-waf-tool-identity-20260906.md#fixture-boundary-r2).

**Spec181 Waf tool identity R1 (2026-09-06): BLOCK at generated build-tool identity.**
Four waflib content/location mutations escape native verification; interpreter
drift is rejected only after the native probe. Focused fixture checks: 5 failed,
70 deselected (0.36 s), no real build/network/qualification process.
Preserve R1, then bind actual selected Waf implementation in the maintained
native identity owner. See [Waf tool identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-waf-tool-identity-20260906.md).

**Spec181 local configuration R1 (2026-09-06): BLOCK at launch identity.**
Six focused mutations of environment, reserved output variable, declared
digest and interpreter bytes reach the forbidden qualification-child boundary
(6 failed, 0.78 s). The runner already uses explicit environment; the missing
check binds its actual values to the declared digest. No qualification child
ran. Preserve R1 before repairing the shared launch-configuration owner.
See [local configuration identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-config-identity-20260906.md).
R2 binds actual launch inputs through one shared builder/gate owner (52 PASS).
R3 verifies real child environment consumption and post-execution identity
failure (61 PASS). R4 adds builder/gate CLI round-trip and rejects changed
configuration before output/children: 62 PASS (5.80 s). Launch configuration
unit CLOSED; T007 remains BLOCK at runtime/input-byte identity planes.

**Spec181 revision 7 (2026-09-06): BLOCK at local configuration identity.**
Committed-source validation is closed in 2628e3d2 (39 focused checks and
the configured checkout PASS). The remaining controlling work binds actual
local configuration, build/runtime dependencies and development delivery.
By owner decision, SIF/replay/Tiger move to the experiment machine and are
not local closure prerequisites. Git merge is deferred until development ends.
See [scope transfer](../specs/181-ndnsf-di-protected-grant-qualification/evidence/development-scope-transfer-20260906.md)
and [current tasks](../specs/181-ndnsf-di-protected-grant-qualification/tasks.md).

**Spec181 local gate identity R8 (2026-09-06): checkpoint hook rejection.**
The local commit hook rejects assistant-directory references in production
source validation. Remove those non-product exclusions and rerun the focused
checks; do not bypass the hook. No checkpoint was created by the failed commit.
R8 removes the exclusions: 39 focused checks PASS (4.46 s), and the actual
configured checkout passes SOURCE_CHECKOUT_OK. The hook remains enabled.
The retry checkpoint succeeds as 2628e3d2; this hook incident is closed.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md#checkpoint-gate-r8).

**Spec181 local gate identity R7 (2026-09-06): source unit CLOSED; configuration BLOCK.**
All 39 focused checks pass (3.44 s), and the configured 1ba99000 checkout
passes exact source validation. Bad preflight identity has no qualification
child/output side effects; mutation during fixture execution yields
UNQUALIFIED while preserving child/cleanup records. Effective configuration,
generated build-tool/runtime bytes and external import bindings remain open.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md#focused-green-r7).

**Spec181 local gate identity R6 (2026-09-06): BLOCK at Python compatibility.**
New symbolic-link checks use Path.is_relative_to, absent from the maintained
interpreter. Focused checks yield 23 failures / 16 passes; the subsequent
read-only checkout probe hits the same AttributeError before qualification.
Preserve R6; use relative_to with ValueError handling, then require focused
success before the next checkout probe.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md).

**Spec181 local gate identity R4 (2026-09-06): BLOCK at Git LFS representation.**
The configured 1ba99000 checkout is rejected because a committed 134-byte LFS
pointer represents a materialized 240376592-byte release archive. Preserve
R4 before adding exact pointer size/SHA-256 verification; do not classify this
as source tampering or a protocol failure. No qualification child ran.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md).
R5 closes exact LFS-byte verification and passes 33 focused checks (2.51 s).
The next configured-checkout rejection is generated Waf tool code; classify
only its exact generated layout under the separate build-tool identity plane,
whose byte binding remains an A05 obligation. Preserve both R5 logs.

**Spec181 local gate identity R1 (2026-09-06): BLOCK at source authority.**
The real local gate accepts a fixture root without a Git HEAD and an invented
40-character sourceRevision, then reports PASS for six fixture children.
All six exits/cleanup records are collected; no network qualification ran.
Validate actual checkout/source identity before any qualification child or
output directory is created, then retain focused regressions for rejection.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md).
R2 adds real Git fixtures and nine identity mutations; all nine reach the
forbidden child boundary instead of being rejected (9 failed, 0.63 s).
No qualification child runs; preserve the RED log and source patch before
adding checkout, index, tracked-byte and untracked-code checks.
R3 passes all nine rejections and existing gate/inventory checks (23 PASS,
2.39 s). Actual configured-checkout and submodule boundaries remain under
focused review; configuration/candidate identity is not yet closed.

**Spec181 native plan closure R1 (2026-09-06): BLOCK at projection behavior.**
Twelve missing header lines close the maintained local native build, including
the extension import/identity check. Focused plan/merge tests then yield
27 PASS / 2 FAIL: COMPONENT_SET postprocessing is rejected, and two PIPELINE
tensors collide in runtime scope with mismatched producer/consumer names.
Preserve R1 before applying the exact parser/scope repair; no qualification
matrix or model run was started.
R2 closes the parser/scope unit: 29 cases / 133 assertions PASS, including
unchanged transport authorization groups. Refresh native identity from its
source checkpoint before advancing the remaining A05 candidate/config audit.
The 1ba99000 checkpoint subsequently passes the maintained native build,
including a fresh extension import and runtime identity receipt (R3).
See [native plan closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-native-plan-closure-20260906.md).

**Spec181 native source diagnostic R3 (2026-09-06): BLOCK at DI projection declaration.**
A checkout refusal left the first native retry on the prior HEAD plus an
explicit source patch; that run is invalid as clean-commit evidence. It
terminated with two compiler errors: NativeCanonicalOnnxAssembler reads
canonicalArtifactName absent from the committed NativeSelectionProjectionV3.
The nine tested files were then matched to 1df718c8 and checkout completed.
Inspect the declaration and assignment path before a fresh recorded retry.
See [framework source closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-framework-source-closure-20260906.md).

**Spec181 framework source closure R1 (2026-09-06): BLOCK at assignment metadata.**
The isolated six-file dependency closure builds the production framework
library. Its reused lifecycle/assignment checks yield 16 PASS / 1 FAIL:
ServiceProvider loses artifactDataName while projecting a structured
assignment set into CollaborationContext. Preserve the R1 source patch and
result, then close the exact Provider transfer before retrying.
R2 carries the root name through single/structured assignments and rejects
conflicting roots. The isolated target links and passes 26 cases / 204
assertions. This framework boundary is closed; full native closure remains.
See [framework source closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-framework-source-closure-20260906.md).

**Spec181 committed native build R4 (2026-09-05): BLOCK at C++ source closure.**
Clean 6c7a0b23 passes configuration and Waf graph creation, then ServiceUser.cpp
fails to compile: AckAuthenticationEvidence is missing, followed by missing
registration and publish-result declarations. The implementation depends on
uncommitted framework declarations/companions. Preserve R4 and close those
exact dependencies before the next clean build; no protocol result exists.
See [committed native closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-committed-native-build-20260905.md).

**Spec181 committed native build (2026-09-05): BLOCK at Waf graph creation.**
Detached d67de87a configures successfully, but tests/wscript references an
untracked native assembly integration source. find_node returns None and Waf
fails before C++ compilation. No protocol result exists; preserve the isolated
checkout and repair the committed source dependency before retrying.
One focused identity regression also fails: changing loaded tests/wscript
bytes with unchanged mtime does not invalidate the native receipt, because
that Waf control file is missing from source fingerprints.
R2 binds the missing control file (70 identity checks PASS) and builds the
integration target, but its seven named assembly cases yield 3 PASS / 4 FAIL.
All four fail at certified recipe_digest validation before ORT loading;
preserve R2 and compare fixture serialization with the production contract.
R3 corrects the old fixture's quoted integer dimensions, leaving production
digest validation intact: 7 cases / 160 assertions PASS. The missing test
source and identity repair are ready for a checkpoint; a fresh committed
checkout must still pass the maintained native build. T007 remains BLOCK.
See [committed native closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-committed-native-build-20260905.md).

**Spec181 A05 qualification inventory (2026-09-05): CLOSED within focused inventory repair.**
The inherited inventory requires Q-C/Q-W but discovers only Spec180 Python
tests, omitting Spec181 protected-grant regressions. Two tests using the real
inventory builder and pytest collection fail; no formal network run started.
R2 repairs those cases (15 PASS); an old count assertion is corrected, and R3
passes 16 checks. R4 then exposes a second boundary: two tests show rehashed
case-source substitution is accepted by the real builder and gate. These use
fixture children, not a network qualification. Preserve R4 before repair.
R5 rejects both substitutions (17 PASS); the old missing-oracle fixture also
changed its source path and is now correctly rejected earlier. R6 keeps the
registered path while withholding its oracle to preserve that check's scope.
R6 passes 18 checks. R7 then finds three unsafe entry IDs accepted by the
validator although the gate joins IDs into output directories; no escaped
write is attempted. Reject these IDs at the inventory boundary before R8.
R8 passes all 21 checks: active scope/collection, registered case source/args,
safe entry IDs, existing evidence/oracle controls and wrapper compatibility.
The remaining A05 native/candidate effective-configuration audit stays BLOCK.
See [qualification scope repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-qualification-scope-20260905.md).

**Spec181 T007 evidence inventory (2026-09-05): CLOSED within document inventory scope.**
The R003 record reports zero Spec181 evidence files, while the current tree
contains 37. Four active evidence records lack an explicit header layer, and
the audit body still describes already-closed T002/shared-runtime gaps. This
invalidates the old completeness claim, not the linked raw test results.
Four layer headers are repaired; the current inventory covers 145 entries,
including both audit roots and one explicit SELF row. Drift/structure checks
PASS, and all 105 Spec180 evidence-file hashes match the before-scan. Raw scan:
ignored workspace temporary directory `spec181-evidence-inventory-20260905-r1/`.
See [complete inventory](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-evidence-inventory-20260905.md).

**Spec181 shared generation worker (2026-09-05): CLOSED by focused repair.**
Two queued coordinator regressions entered the model after cancellation or
deadline. The existing pre-run cancellation check passed (1/3 cases PASS,
14/18 assertions PASS). Propagating the shared guard through the registered
runtime/worker and state staging repairs the boundary: rebuilt 48 cases /
366 assertions PASS. T007 remains BLOCK for its remaining audit obligations.
See [generation worker authority](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-generation-worker-20260905.md).

**Spec181 shared preparation control R1 (2026-09-05): CLOSED as startup error; R2 focused PASS.**
The isolated launcher omitted the empty output directory; `validate_inputs`
raised `OUTPUT_ROOT_MISSING` before MiniNDN startup. No protocol result exists.
The unified native build passed. R1 is preserved; fresh R2 passed the protected
P-256 control with four verified Providers and seven collected child exits.
See [shared preparation closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-shared-preparation-20260905.md).

**Spec181 T002 native handler observation (2026-09-05): CLOSED; early result INVALID.** A focused test
was started before the repair build completed and ran the previous binary.
R2/green.log is preserved. The original build completed (59.612s), then the rebuilt
binary passed 46 cases / 242 assertions in a new log. The early result is a
validation orchestration error, not a protocol result.
See [native handler closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-native-handler-closure-20260905.md).

**Spec181 T002 native prepared output binding (2026-09-05): CLOSED (focused repair).** The production
prepared-runner validator accepts a Merge output budget changed from the sealed
Selection's K=300 to K=1. R1 fails one of two assertions after a passing positive
control. Exact output shape/type binding now rejects the mutation; rebuilt focused
checks pass 46 cases / 242 assertions. Source closure remains pending; see
[native handler closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-native-handler-closure-20260905.md).

**Spec181 T002 native Merge contract (2026-09-05): CLOSED (focused repair).** Direct production-runner
checks pass the numerical controls but expose four missed rejections: unknown
postprocess identity, numeric suffix, trailing shape delimiter, and wrong output
dtype. R1 is preserved (3/6 cases, 22/26 assertions passed). Rebuilt repairs pass
6 cases / 26 assertions; related evidence/readiness checks total 10 cases / 69
assertions. Handler source closure remains open; see
[native Merge closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-native-merge-closure-20260905.md).

**Spec181 T002 native worker authority (2026-09-05): CLOSED (focused repair).** Four real-worker
regressions show that cancellation/expiry after preparation or during compute
still returns results and retains the registered plaintext lease. Grant
verification is valid initially; the missing boundary is worker consumption.
R1 is preserved. Request guards now fence preparation, compute, cached results,
events, publication, and return; rebuilt focused checks pass 51 cases / 280 assertions.
T002 source closure and unified production rebuild remain open; see [worker authority](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-worker-authority-20260905.md).

**Spec181 T001 request lifecycle (2026-09-05): CLOSED.** Twelve registered-handler
regressions show that cancellation or Selection deadline expiry before/during
grant fetch or after preparation still reaches model execution. Grant expiry
alone does not enforce request lifetime. Preserve the first red run before
repair. Four final regressions also expose a missing comparison between the
grant-reference and Selection policy snapshot. Both repairs pass 151 focused
regressions and six real Python process cases (24 exits collected). All red
runs are preserved. T001 acceptance is complete; T002/T007 remain open. See
[request lifecycle](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-request-lifecycle-20260905.md).

**Spec181 T001/T002 maintained process integration (2026-09-05): focused defects CLOSED.**
R1 native fixtures build, but NFD's own management FIB registration fails
because the test configuration omits management authorization. The requester
then receives connection refused; no grant verification occurred. Preserve
the r1 raw run. R2 fixes NFD but requester bootstrap requires a real Controller
for NAC public parameters. Separately, ten successful P-256 runtime calls leak
24560 bytes / 630 allocations under ASAN despite the Zeroized state. Both
boundaries are recorded before repair. R3 fixes the EC ownership leak: ten
P-256 calls pass ASAN. The real Controller becomes ready, but requester
publication readiness times out before any Provider result; preserve r3
before instrumenting that boundary. R4 NDN logs/stack locate the wait in
ServiceUser construction (NAC decryption key): the fixture needs its own
requester policy and certificate bootstrap. R5 passes five cases; Python's
wrong-recipient rejection is correct, but the test incorrectly requires a
Core wire prefix on an internal typed exception. Preserve r5 and correct
the scoped assertion without synthesizing a Core response. R6 passes all 11
checks (10 real-process cases plus ASAN), with all 40 child exits collected.
T001/T002 full acceptance and T007 remain open. See
[process integration](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-t002-process-integration-20260905.md).

**Spec181 T002 P-256 production path (2026-09-05): focused defects CLOSED.**
R1 proves the runner overwrites an explicitly configured recipient-key map
with the Ed25519 offer-key map. Four other checks stop in fixture key
generation because this cryptography installation requires an explicit backend;
they do not prove a production refusal. R2 fixes the fixture: three actual
entry regressions fail (requester loader, Python Provider loader, map override),
and two wrong-curve rejection checks pass. R3 repairs those entries: 130 checks
pass, but the positive P-256 case reaches the production envelope creator and
fails because its EC key generation also omits the required backend argument.
All three raw results are retained. R4 repairs the production key generation;
134 focused checks pass, including bounded private-file rejection. The unified
native rebuild and a fresh four-recipient P-256 Y-B control pass: four native
grant verifications, terminal numerical match, seven collected child exits,
and empty staging. Full T001/T002 acceptance remains open.
See [P-256 production path](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-p256-production-20260905.md).

**Spec181 T002 recipient credentials (2026-09-05): focused defect CLOSED.**
The production factory accepts only Ed25519 private keys although T002 and
the native verifier also support EC P-256 envelopes. The rebuilt credential
regression runs eight cases; only P-256 loading fails before grant acquisition
with `provider recipient private key is not Ed25519`. The r1 build and RED
logs are retained. R2 loads validated P-256 PEM through the production loader;
all eight focused checks pass, including wrong-curve and permission rejection.
P-256 network acceptance and full T002 closure remain open. See
[recipient credentials](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-recipient-credentials-20260905.md).

**Spec181 T002 helper lifecycle (2026-09-05): focused lifecycle defects CLOSED.**
The native assembler waits synchronously for its helper, ignores request and
operation deadlines during assembly, observes grant cancellation/expiry only
after slow work, and can activate helper output beyond the role envelope.
Six real-process regression failures are retained in
`spec181-t002-helper-20260905-r1/red.log` in the ignored workspace temporary
directory. R2 builds but 14 native checks stop at the loader: the old installed
framework lacks `streamCancelled`. R3 binds the test executable and its
environment to the current build library: 26 focused checks pass. A new
source-fetch cancellation regression then proves that the parent recreates
the erased plaintext directory. R4 serializes protected staging writes
with runtime cleanup; 27 focused checks pass, including the new race. The
new RED log is retained in r3. The final unified native rebuild and a fresh
protected Y-B control pass: four actual grant verifications, three ORT CPU
roles plus native Merge, verified terminal output, and empty staging after
all seven child exits are collected. Full T002 acceptance remains in progress. See
[T002 helper lifecycle](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-helper-lifecycle-20260905.md).

**Spec181 T003 assembly parity (2026-09-05): focused build defects CLOSED.**
The fixed-vector Python lane passes 8 cases; the native lane fails 8 checks
because its required current-source test executable is not built yet. This
is a test-input boundary, not an assembly or protocol rejection. R2 compile
also fails before execution because the manual command omitted the installed
NAC-ABE package's `NAC_ABE_CMAKE_BUILD` definition. R3 also lacks the maintained
framework include path; r4 replaces the manual command with a focused Waf
target using the existing dependency configuration. R4 compiles but exposes
the framework's NDNSD link dependency; r5 adds that configured dependency.
R5 build passes; the final 19 parity checks pass, including real ORT CPU
execution and unchanged negative-cache state. Fixed-vector regeneration is
byte-identical. T003 is complete at its focused scope; T007 remains BLOCK. Preserve
`spec181-t003-assembly-20260905-r1` and build the production-entry fixture
before another attempt. See
[T003 assembly parity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t003-assembly-parity-20260905.md).

**Spec181 T005 evidence preservation (2026-09-05): focused defects CLOSED.**
Five focused checks expose the legacy driver's destructive attempt handling,
runtime-dependent entry, and continuation after matrix failures. No network
processes are started. The repair retires this unsafe automatic retry path
and makes the maintained matrix stop at its first failed subcase. Raw RED
output is retained under `spec181-t005-evidence-repair-20260905-r1` in the
ignored workspace temporary directory. R2 passes 118 focused checks after
retiring the legacy entry and stopping on the first matrix failure. This is
not formal matrix qualification; T005 remains NOT PROVEN. See
[T005 evidence repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-evidence-repair-20260905.md).

**Spec181 T006 positive control (2026-09-05): cold dependency timeout CLOSED.**
All four native grants verify, but r8 Merge's first tensor-manifest fetch
expires at the fixed 10 s no-progress bound while its producer finishes cold
model preparation. User then times out. The three actual grant negatives
pass. After binding the data wait to the configured request budget while
retaining the existing hard deadline and cancellation, r10 completes the
protected native control; a measured dependency wait is 13.97 s. The final
r11/r12/r13 negatives also pass with complete process collection. T006 is
complete at its focused scope; T007 remains BLOCK. See
[T006 production repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t006-production-repair-20260905.md).

**Spec181 T006 production rejection (2026-09-05): false-positive oracle CLOSED.**
Eight focused regressions fail: the configured requester seam publishes no
mutation, invalid mutation/epoch settings are admitted, and User-local
exceptions or markers can masquerade as Provider rejection. No selected
Provider network rejection is established by those old probes. See
[T006 production repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t006-production-repair-20260905.md).
After repair, 62 focused checks pass. The separate runner check retains one
obsolete expectation that a User-local Y-N-E probe should report PASS; that
test is corrected; the updated Python group has 150 PASS. A separate C++
harness link omitted the store source; the next command used a nonexistent
shortened filename. The verified source is NativeProtectedArtifactStore.cpp;
use a new directory for the corrected harness command. The unified native
build has independently completed successfully.
r4 C++ harness passes 22 cases. The r5 live entry stops before network
creation because Y-B inputs omit Y-N's FullModel role. Use the verified
five-role Y-N inputs with an explicit protected epoch for subsequent variants.

**Spec181 T004 lifecycle acceptance (2026-09-05): focused defects CLOSED.**
R1 failed before the waiter because the fixture had no running Controller
serving AA public parameters. R2 corrects that startup order and reaches the
real Provider waiter: an 80 ms wait returns false in about 6 us before run,
with SPEC181_PROVIDER_READINESS_PREMATURE_TERMINAL. The initial non-running
state was incorrectly terminal. R3 retains an OUTPUT_ROOT_MISSING preflight
failure. After the native fix/rebuild, r6 waits 83 ms and starts/stops the real
Provider; r4 reaches Controller readiness at 12.39 s; r5 cancels an active
Core probe in 2.3 ms without hot spinning. All three probes exit 0 after
process collection and network cleanup; six Core checks also pass. T004 is
complete at its focused acceptance scope; T007 remains BLOCK. See
[T004 lifecycle acceptance](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t004-lifecycle-acceptance-20260905.md).

**Spec181 T002/T004 native repair (2026-09-05): focused defects CLOSED; tasks OPEN.**
Retained failures identify group digest format, grant forwarding hint, model
basename, premature readiness, and debugger exit-code boundaries. Live-r7
uses ordinary process commands and the same rebuilt source: native protected
Y-B returns PASS/exit 0, with three ciphertext files and no ONNX plaintext or
staging remnants after cleanup. Production negatives, cancellation/resource
acceptance, source checkpoint closure and T007 remain open. See
[native launch repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-native-live-repair-20260905.md).

**Spec181 T002 production wiring (2026-09-05): build defect CLOSED; T002 OPEN.**
The first compile failed on the installed ndn-cxx forwarding-hint API. A
separate retained r2 build passes native/library/extension identity checks,
and 3 rebuilt-extension tests consume the 9 grant vectors. Real Provider
network and ORT lifecycle acceptance remains open. See
[T002 production repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-production-repair-20260905.md).

**Spec181 T002 native runtime repair (2026-09-05): focused defects CLOSED.**
18 native focused tests pass with real verification, managed content keys,
deadline/cancellation checks and retryable cleanup. Initial compile, linker
and consumption/deadline failures remain preserved. Factory, storage AEAD
and real network acceptance still keep T002 open.
See [T002 runtime repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-runtime-repair-20260905.md).

**Spec181 T001 registry repair (2026-09-05): focused defects CLOSED.**
100 focused tests plus 7 inherited grant tests pass for pinned registry
policy, private-key matching, distinct issuer/publication identities and
the final published-root allowlist. Initial RED and Python 3.8 compatibility
failures are retained. T001 network and lifecycle acceptance remains open.
See [T001 registry repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-registry-repair-20260905.md).

**Spec181 T001 Provider repair (2026-09-05): focused defects CLOSED.**
70 focused tests pass for authorization before preparation, in-memory keys,
on-disk AEAD loading, model/weights cleanup and registered-handler failures.
Registry-policy wiring and real network integration still keep T001 open.
See [T001 Provider repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-provider-repair-20260905.md).

**Spec181 T001 lifecycle repair (2026-09-05): focused defect CLOSED.** Five
RED failures are repaired; 19 focused tests pass for in-memory key leases,
duplicate protection, complete cleanup and private/symlink-safe files.
Provider integration remains open; this does not close T001.
See [T001 lifecycle repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-lifecycle-repair-20260905.md).

**Active Spec181 audit (2026-09-05): BLOCK.** Native protected runtime wiring,
production-path negative validation and assembly parity remain unproven;
the active Context Mode plan link has been repaired. Latest retained Spec181 Y-B log
reports `CASE_RUNTIME_PROCESS_START_FAILED:control`, not a protocol result.
See [Spec181 audit repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/audit-repair-20260905.md).
Focused repair checks are allowed; full qualification requires a fresh audit PASS.

**Current controlling repair (2026-09-05):** Y-N negative verdicts accepted
unrelated exceptions as PASS. The first focused regression reproduced 12
failures; the repaired User/runner/application/build-guard set passes 183
focused tests. The unified build also exposed a stale legacy RUNPATH in
`build-system-j2`, despite its name. See
[negative verdict and build repair](../specs/180-ack-driven-cross-model-qualification/evidence/t011-negative-verdict-repair-20260905.md).
FR-008 still lacks the production authenticated, recipient-encrypted grant
path and operator-authorized issuer configuration; see
[protected-grant gap](../specs/180-ack-driven-cross-model-qualification/evidence/t008-protected-grant-gap-20260905.md).
Older PASS labels below must not be reused as safety evidence. The r42 startup
observation remains useful but does not close this semantic verdict defect or
the missing FR-008 protected execution path.

| ID | Observed | Scope | First failing boundary | Disposition | Durable record | Raw run data |
| --- | --- | --- | --- | --- | --- | --- |
| `SPEC180-Y-N-R42-EXTENSION-CWD` | 2026-09-05 | Spec180 current-source extension rebuild | `pythonWrapper/setup.py` was invoked from the repository root, so its relative C++ source path could not be found | `CLOSED as command-invocation error`; compiler exited 1 before producing an artifact | [`t011-y-n-live-current-20260905-r42-extension-build-cwd.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r42-extension-build-cwd.md) | no raw run; command output is preserved in the task log |
| `SPEC180-Y-N-R41-I-EXTENSION` | 2026-09-05 | Spec180 focused Y-N-I current-source check | Loaded Python extension/framework artifact identity before interpreting the live protocol result | `CLOSED by r42`; rebuilt artifacts contain the current source and r42 logged the marker sequence | [`t011-y-n-live-current-20260905-r41-extension-check.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r41-extension-check.md), closure [`t011-y-n-live-current-20260905-r42-i-only.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r42-i-only.md) | ignored workspace temporary run `spec180-yolo-y-n-current-20260905-r41-i-only/` |
| `SPEC180-Y-N-R40-PATH` | 2026-09-05 | Spec180 diagnostic I-only | Temporary run path before the maintained helper completed | `CLOSED as command-path error`; the command used a mistyped directory and was interrupted with exit 130 | [`t011-y-n-live-current-20260905-r40-path-preflight.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r40-path-preflight.md) | preserved partial data under ignored workspace temporary `spec180-diagnostic-path-error-r40/` |
| `SPEC180-Y-N-R39-I` | 2026-09-05 | Spec180 local MiniNDN Y-N | Controller `PUBPARAMS` readiness before Y-N-I reached provider execution | `CLOSED by r42 for the focused I-only boundary`; r42 reached readiness and provider execution with the rebuilt current artifacts; the full matrix remains open | [`t011-y-n-live-current-20260905-r39.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r39.md), closure [`t011-y-n-live-current-20260905-r42-i-only.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r42-i-only.md) | ignored workspace temporary run `spec180-yolo-y-n-current-20260905-r39/` |
| `SPEC180-Y-N-R37-P-PREFLIGHT` | 2026-09-05 | Spec180 diagnostic P-only | MiniNDN root/user-namespace preflight before NFD creation | `CLOSED by r38`; diagnostic command omitted `unshare -Urnm` and exited 1 | [`t011-y-n-live-current-20260905-r37-p-only-preflight.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r37-p-only-preflight.md), closure [`t011-y-n-live-current-20260905-r38-p-only.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r38-p-only.md) | ignored workspace temporary P-only attempt `spec180-yolo-y-n-current-20260905-r37-p-only/` |
| `SPEC180-Y-N-R36-P` | 2026-09-05 | Spec180 local MiniNDN Y-N | Controller `PUBPARAMS` readiness before Y-N-P reached the ACK-closed boundary | `UNQUALIFIED`; Y-N-O/C/R/I/E/L passed, Y-N-P was not proven | [`t011-y-n-live-current-20260905-r36.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r36.md) | ignored workspace temporary run `spec180-yolo-y-n-current-20260905-r36/` and console log with the same run-id |
| `SPEC180-Y-N-R35-P-E` | 2026-09-05 | Spec180 local MiniNDN Y-N | Controller `PUBPARAMS` readiness before either negative case reached the ACK disposition path | `UNQUALIFIED`; Y-N-O/C/R/I/L passed, Y-N-P/E were not proven | [`t011-y-n-live-current-20260905-r35.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r35.md) | ignored workspace temporary run `spec180-yolo-y-n-current-20260905-r35/` and console log with the same run-id |

Historically, `r42` was recorded as closing its artifact-identity and focused
readiness occurrence. Its rebuilt framework library and Python extension were checked,
and the I log shows the Controller
issuing `registering prefix: /example/controller` and the six NDNSF filters,
followed by successful root and `PUBPARAMS` registration, the readiness probe,
and the expected Y-N-I execution markers. This is focused T011 evidence only;
the full Y-N matrix and T014 remain open. This does not qualify the subsequently
changed readiness implementation or negative oracle, nor prove the complete
dependency closure now checked by the unified build. The earlier r39 I
occurrence was closed only at that historical boundary; its bytes are preserved.

The earlier `r36` result likewise showed the Controller
issuing `registering prefix: /example/controller` and the six NDNSF filters,
but the first Face connection closes before the root registration completes;
the reconnect installs only the six NDNSF routes. There is no successful
Controller-prefix registration and no `PUBPARAMS` filter before the Python
readiness timeout. This is classified as a startup/transport boundary failure,
not as an ACK disposition failure. r35 remains relevant as the prior broader
P/E occurrence.

The ordinary `ConfigManager` message about a missing `/etc/ndn/ndnsf.conf` in
the child logs is ambient diagnostic noise for this run; it is not the
controlling failure because the successful subcases contain it as well.

## Historical pointers

These records remain useful when the current failure is related to their
boundary, but they do not advance the active gate by themselves:

- [`t013-controller-pubparams-readiness-current-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t013-controller-pubparams-readiness-current-20260904.md): why the real AA `PUBPARAMS` readiness barrier exists and why the old exact-SIF result was invalidated.
- [`audit-revision123-design-code-conformance-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/audit-revision123-design-code-conformance-20260904.md): revision-123 design/code findings and their evidence boundary.
- [`t014-tiger-path-audit-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t014-tiger-path-audit-20260904.md): Tiger-path audit block and the reasons implementation checks were not qualification evidence.
- [`t013-supervision-repair-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t013-supervision-repair-20260904.md): supervision and cleanup caveats retained after the repair.

## Failure-record contract

Every failed, blocked, or `UNQUALIFIED` command that can affect task order
must produce or update a durable record before the next retry. The record must
contain:

1. a unique run/failure ID and UTC/local date;
2. the exact source/candidate/config identity and command;
3. the first failing boundary, exact marker or error, and child exit status;
4. the raw-log path plus a compact, secret-free excerpt or structured summary;
5. the affected Spec task/gate and any candidate/evidence invalidation;
6. the next allowed action and the condition that closes the failure.

Keep large logs, private keys, credentials, and transient sockets out of Git.
Use a new ignored workspace temporary directory named with a unique run-id for
each attempt; never overwrite a prior run. The durable evidence record must be
sufficient to understand the failure when the transient directory is later
unavailable.

## Closing an entry

Change the disposition only after a fresh run reaches the same boundary and
produces the required closing marker. A focused unit test can close an
implementation defect, but it cannot close a live, SIF, or Tiger gate unless
the active Spec explicitly defines that test as the gate's evidence.
# NDNSF Failure Log

## 2026-09-06 — UAV API compatibility audit: static authorization expires without renewal

- **Symptom**: an application with unchanged static permissions can compile with the new APIs but lose authorization at the Controller status's 24-hour boundary. Re-fetching the status does not extend its fixed validity window. Source probes also reproduce seven old-call compilation failures: deleted public large-response helper, three positional aggregates, two Hybrid member pointers and one NAC ParamFetcher member pointer.
- **Cause**: Controller initialization/epoch advance sets `m_policyValidUntilMs`; ordinary `getPolicyStatus`/Interest handling does not renew it, while new runtime enforcement rejects expired status. Separately, the helper removal, inserted fields and overload additions break specific source forms. Asynchronous DKEY bootstrap and ACK payload redaction introduce further behavioral migration requirements.
- **Disposition**: audit only, production fixes remain open. Do not present prior short MiniNDN scenarios as static long-running compatibility acceptance or restore insecure fallback behavior. Prioritize safe immutable-status renewal, production readiness semantics, then source compatibility/migration.
- **Evidence**: `specs/179-request-scoped-confidentiality/evidence/uav-api-compatibility-audit-20260906.md`; `results/api-compatibility-audit-20260906/summary.json` has 40 compilations (17 historical and 6 official controls pass; current 10 pass/7 compatibility failures). Six unmodified historical Apps/examples compile. A separately linked production expiry-boundary probe reports lifetime86400000, allowed-before1, allowed-at-expiry0, `controller_status_expired`, expired-refresh-accepted0. Its exit0 means defect reproduced, not fixed; no actual 24-hour network soak was executed.
- **Lesson**: preserve separate source, ABI, protocol, startup and long-running behavior gates. Applications that do not call new authorization APIs still enter the new runtime defaults.

## 2026-09-06 — API audit probe setup produced false compatibility failures

- **Symptom**: initial typed helper probe failed on both revisions (`std::string` lacks `ParseFromArray/SerializeToString`); NAC probes failed with unknown ParamFetcher/PublicParams. Trying a different include order alone did not solve the NAC failure.
- **Cause**: the synthetic payload did not satisfy the pre-existing template contract. Probe compiler paths allowed NAC algo headers' bare `common.hpp` to resolve to NDNSF's same-named header. These were invalid audit controls, not new library defects.
- **Fix**: use a minimal protobuf-shaped compile-only payload; put the NAC source/package directory ahead of NDNSF paths and use NAC's CMake header mode. Preserve initial logs under `results/api-compatibility-audit-20260906/setup-attempts/`, then rerun the entire matrix with at most two compiler processes. Final historical controls all pass, and the seven current failures are attributable to actual API changes.
- **Lesson**: never call a compiler red an API regression until the identical source compiles on the declared baseline with correctly isolated headers.

## 2026-09-05 — T022 official-merge segmentation regression setup

- **Symptom**: first new Producer segmentation test build fails because the default-template factories are protected. Two guessed inspection paths did not exist; the actual campaign is `scripts/spec179_minindn_campaign.sh` and Segmenter belongs to installed ndn-cxx. Initial T022 placement also made the structure scanner warn about task order.
- **Fix**: use explicit Data templates in tests without changing production access; locate exact files and move T022 after T021. Preserve the failed build log under `results/spec179-official-merge-20260905/nac-red-build.log`; retry at unchanged-j2.
- **Lesson**: test public behavior through explicit inputs; do not expose production helpers for convenience. Use file discovery instead of guessing script paths. This compiler failure is a test setup defect, not the expected pre-merge behavioral red.
- **Behavioral red**: fresh85547eb build plus four new cases gives12/15 pass,3 failed cases/5 failed assertions, exit201. CP/KP ignore128-byte limits (1500-byte data, single CK segment); normalizeCkKey collapses distinct segment-named objects and the second warm-cache decrypt returns wrong plaintext. Existing object/exact-segment retrieval and invalidation/reentry tests pass. Official58f3948 merge is conflict-free; green results pending.
- **Merged green**: Experimentalc3aafa6 retains85547eb and official58f3948 ancestry. The same15 cases all pass, including both segment-limit tests and distinct-CK plaintext. Separate prefix installed with f5cb1ec8 library hash; full NAC/native/network qualification continues.
- **Dependency gates**: full46/46,4284 assertions; installed-prefix26/26,1082 assertions. An initial launcher unittest command treated tests/minindn as an importable package and failed2 imports; rerun with PYTHONPATH=tests/minindn and explicit module names. Preserve the import-error log separately from actual launcher results.
- **Launcher runner correction**: that import-path retry executes0 unittest cases, so it is rejected as vacuous. These are pytest function tests; inspect their declared runner and execute python3 -m pytest on both exact files. Neither the import failure nor the zero-test exit0 is a launcher green gate.
- **T022 closure**: merged15/15, full NAC46/46 (4284 assertions), installed26/26 (1082), clean native183 unit/74 integration (11998/1297), pytest28/28; full18 MiniNDN scenarios,188 assertions and both dedicated User grant gates at clean cdd8e55a.33 hashes match disk, driver/all CLIs exit0. Build28m9.720s at-j2; no runtime source changes beyond the two official NAC files. Preserve the compiler/import/zero-case and behavioral reds. T014 publication is deferred, not failed or satisfied by the local merge.

## 2026-09-05 — NAC upstream provenance inferred from a remote alias

- **Symptom**: the first T014 delivery update called suraviregmi/NAC-ABE the original project and inferred12 missing prerequisites from its stale master.
- **Cause**: verified remote SHAs but did not first verify GitHub fork parent/source metadata. The alias `suravi` did not establish official ownership.
- **Correction**: GitHub API confirms both remotes fork UCLA-IRL/NAC-ABE. Fetching official master58f3948 into an isolated clone shows fork master2 ahead/3 behind and Experimental6 ahead/3 behind. The missing changes are maxSegmentSize propagation and removal of segment stripping, plus their merge. Corrected all current delivery/plan/task/audit claims; the preceding entry is historical and its original-project12-commit statement is superseded here.
- **Evidence**: both isolated no-commit merge previews pass without conflicts, changing only cache-producer.cpp and consumer.cpp; previews aborted afterward. Candidate trees and exact source diffs are recorded in `specs/179-request-scoped-confidentiality/evidence/nac-abe-official-comparison-20260905.md`. No candidate runtime qualification, actual NAC branch update, push or PR occurred. User explicitly requests no PR.
- **Lesson**: establish official repository identity from parent/source metadata, then fetch live refs and compare both ancestry directions. A clean merge proves textual compatibility only. A multi-file documentation patch with one mismatched context was rejected atomically; split it into verified exact-context updates without altering runtime files.

## 2026-09-05 — T014 upstream package described an incomplete, ambiguous delivery

- **Symptom**: the earlier package omitted NAC T019/T020, called personal fork master upstream, mixed ParamFetcher API changes into a no-API-change PR and treated the tested OpenABE worker as independently optional.
- **Cause**: delivery prose was not refreshed after dependency compatibility repairs or verified against actual remote/base ancestry.
- **Fix**: retain the old draft as explicitly superseded; pin all four commits through85547eb, map every public surface, document ABI/lifetime limits, provide a concrete fork PR draft, and distinguish the12 prerequisite commits between original master and the tested base. A standalone bundle passes verification, fresh clone, exact head/tree comparison and fsck. T014 stays open for publication authorization, upstream acceptance and rebuilt gates.
- **Tool deviations**: a query guard rejected low-entropy `T014`; corrected to the exact feature basename before authoritative reuse. Broad CodeGraph exploration returned unrelated version symbols; after the required attempt, exact source-string verification established callers. apply_patch rejected delete/add operations on one path atomically; a separate current report and historical pointer avoid destructive replacement. No runtime files changed.
- **Lesson**: an upstream package must identify the destination, prerequisites and complete tested revision; a local bundle or fork push does not prove upstream acceptance. Use a high-entropy feature identifier and update each path once per patch.

## 2026-09-05 — Provider online authorization was absent from MiniNDN grant coverage

- **Area**: Spec179 T021, Controller service-offering permissions.
- **Finding**: existing network grants hardcode User/B and `/PERMISSION`; Provider component policy assignment and network revocation do not establish first-grant service execution. Controller example's grant timer cannot select `/SERVICE`, and Provider example lacks the User example's explicit post-startup permission renewal.
- **Repair**: explicit User/Provider grant-role option (default User), Provider App-owned renewal timer, separate Provider normal/late first-grant scenarios with targeted traffic and unaffected control. Pure evaluators retain target/control failures and reject early service, wrong role/provider, absent renewal and missing late timeout ordering.
- **Evidence**: new evaluator before implementation fails11/11 because no evaluator exists (`provider-grant-evaluator-red.log`); after implementation11/11 pass. Combined launcher gate initially25/26 passes; sole failure is the old exact User-only guard-error string. Parameterize the host-namespace guard test for both roles; final27/27 pass (`provider-grant-launcher-final.log`). Native rebuild and network results pending under `results/spec179-nac-compatibility-20260905/`.
- **Lesson**: Controller policy mutation, runtime permission installation, key readiness and actual service execution require distinct evidence on each role. A User grant cannot qualify Provider service-offering authorization.
- **First network red**: normal and late Provider runs both complete but exit4/gatefalse (`provider-campaign-first/`, driver exit1); User compatibility control passes. Benchmark paths ignore `--known-provider-ids`, so Provider/A serves requests intended for B, including pre-grant successes. User compares provider/service records when deciding DKEY refresh, so an added Provider route incorrectly refreshes an unchanged User service attribute. Normal control31/32 and target31/32 also retain a transition timeout. Provider status-advance permission revalidation legitimately precedes the manual timer in the normal case, contradicting the initial test assumption.
- **Follow-up repair**: benchmark calls use the existing explicit-provider overloads, preserving built-in/custom selection; User DKEY change detection compares service sets, with an added route-only Controller integration regression. Evaluate actual post-grant Provider permission-fetch events and the later idempotent App timer separately. Use the existing grant probe's250ms status cadence; retain all terminal failures and document that version changes can cancel in-flight work, rather than claiming uninterrupted traffic at arbitrary timing. Rebuild/native/network verification pending.
- **Repaired native checkpoint**: build exit0 in4m27.010s at-j2; six dependency closures pass; launcher28/28, unit182/182 (11971 assertions), integration72/72 (1281 assertions) pass. Final18-scenario network acceptance pending.
- **Second network red**: at925ec3a9 both Provider variants pass every check except the exact selected Provider:17/17 and22/22 post-renewal successes still mix Provider/A and B. The App now passes B correctly; `handleRequestAckByName` checks Controller permission but omits the request's explicit Provider set. Stop the campaign driver after retaining both failures (driver143); the in-flight User control also completes successfully. This partial cohort is not a final18-case gate.
- **ACK fix**: reject decoded ACKs outside a nonempty pending-call Provider set before status hints, ACK metrics or selection; keep empty-list discovery behavior. Complete any tracked decrypt accounting on rejection. A new native case verifies FirstResponding, RandomSelection and AllSelected refuse another Controller-authorized Provider and select the requested one. Rebuild and full final native/network gates pending.
- **ACK native checkpoint**: build exit0 in8m2.858s; six closures pass; unit183/183 (11998 assertions), integration72/72 (1281 assertions) pass. Final18-case network cohort still required.
- **T021 closure**: final `campaign-ack-final/` at clean994018ac passes18/18 scenarios,188/188 scenario assertions and both dedicated User grant gates. Driver and all CLI exits are0; all33 artifact hashes match disk. Provider target/control successes are17/17 +32/32 and22/22 +65/65; User grants10/10 +24/24 and21/21 +60/60. Planned restart/outage role exits-2 are explicitly covered; other role exits0. `final-network-verification.log` and `evidence/provider-online-grant-20260905.md` are authoritative. Earlier failures are not relabeled.

## 2026-09-05 — NAC-ABE compatibility review exposes dependency boundary defects

- **Area**: Spec179 T020, NAC-ABE Experimental compatibility and callback ownership.
- **Symptoms**: two new real CK fan-out tests abort with memory-access violations when success/error callbacks call `clearCache`; late parameter replies replace new state (two failed assertions); current Authority bytes are returned under an unavailable old version (two failed assertions); wrong/empty CP/KP keys decrypt after another key warmed the singleton cache (four failed assertions).
- **Root causes**: application callbacks invalidate the live waiter-map iterator; ParamFetcher fences neither fetch/validation nor retry generations; Authority reflects a requested exact name instead of its actual generation; the inherited crypto cache is indexed by ciphertext alone. The latter predates both reviewed commits but prior tests manually cleared it before negative-key checks.
- **Repair**: detach CK batches before callbacks and check generation between waiters; fence parameter delivery, validation and retries, and commit decoded/name/digest-checked candidates atomically; construct canonical Authority names; bind the crypto cache to a hashed length-delimited scheme/parameters/private-key/ciphertext tuple. Restore the original no-argument ParamFetcher entry and document class-layout rebuild requirements and silent cancellation semantics. Verification is in progress.
- **Evidence**: `results/spec179-nac-compatibility-20260905/red-{reentry-success,reentry-error,params-authority,cache}.log`; durable report `specs/179-request-scoped-confidentiality/evidence/nac-abe-compatibility-review-20260905.md`.
- **Test/tool findings**: first full NAC run passed41/42; the sole failure was an existing lifecycle probe requiring an unset role variable. Default it to the User role while retaining explicit role validation; CTest also needs the fixture directory and a supported report option. An initial class-layout probe omitted ndn-cxx at link time; relink with its pkg-config libraries. A provisional focused run started before final test linking and used the previous test executable, so it is not the final gate.
- **Inspection failure**: `objcopy --dump-section` without an explicit output object rewrote both input ELF files during an installed-prefix test. Stop that test (exit143, `nac-installed-interrupted.log`), relink from unchanged objects and reinstall to restore the original hashes, then repeat installed-prefix execution. Use read-only ELF readers or disposable input copies for future section comparisons; never inspect live libraries with an in-place tool.
- **Dependent build failure**: the mandatory layout rebuild hit GCC9 `internal compiler error: in ggc_set_mark, at ggc-page.c:1547` in system `basic_string.h`, while compiling `HybridMessageCrypto.cpp` for integration-tests (`ndnsf-build.log`, exit1). Retain completed objects and retry once at the same `-j2`; repeated compiler failure requires the already established Clang10/system-binutils fallback. This is not an executed runtime test failure.
- **ABI rebuild finding**: the retry linked successfully in6m35.140s, but object timestamps showed Controller flow and generic API tests still dated15:24–15:27, before the17:22 parameter-header layout change. A successful incremental link is insufficient: invalidate stale objects for the selected framework/tests/three Apps, retain the exact path list, and rebuild again at `-j2`. Unrelated build targets are excluded. No tests from the stale-object link count as acceptance.
- **Compiler fallback**: after invalidating90 selected stale objects, GCC again crashed in `basic_string.h` and produced a non-constant assembler `.size` expression (`ndnsf-build-fresh-objects.log`, exit1). Stop GCC retries. Configure a new `build-clang-spec179-nac-compat` with explicit Clang10, system binutils, the exact NAC prefix and `-j2`; no cached GCC objects enter that build.
- **Strict compiler finding**: Clang rejects the unused `this` capture in the User status-restore validation-error callback (`clang-build.log`). The mirrored Provider callback has the same unused capture. Remove only those captures; retain `-Werror` and the callback body. This is the only NDNSF runtime source change in the dependency review.
- **Lesson**: tested cancellation must include callback reentry and validator latency; cache tests must retain a warm cache for unauthorized callers. Source-compatible calls do not establish binary-layout compatibility.
- **Native checkpoint**: NAC42/42 full cases,20/20 installed-prefix cases; clean Clang NDNSF build exit0 in21m10.471s, unit182/182 (11971 assertions), integration72/72 (1278 assertions), all six target dependency closures pass. Complete16-scenario MiniNDN rerun pending.
- **T020 closure**: fresh16/16 MiniNDN runs completed with CLI exit0,161/161 scenario assertions and both dedicated User grant gates. All33 artifact hashes match disk and every manifest binds clean de1eb508. Planned Provider restart/Controller outage exits-2 are checked by their scenarios. See `campaign-verification.log` under the evidence root. Provider online first grant remains a separate T021 coverage gap.

## 2026-09-05 — NAC dependency test build retained a removed Boost prefix
- **Area**: T019 dependency regression build
- **Symptom**: after correcting a missing test error-header include and parenthesizing a Boost assertion message, test compilation succeeded but linking required absent `/usr/local/lib/libboost_unit_test_framework.so.1.82.0` (`gates/nac-revocation-red-build2.log`).
- **Root cause**: enabling tests reused stale Boost CMake cache entries in the existing exact-prefix build directory.
- **Fix**: unset only `Boost_*`/`boost_*` cache entries, configure `BOOST_ROOT=/usr` and `Boost_NO_BOOST_CMAKE=ON`; verify all resolved Boost libraries point to system1.71 before rebuilding with `-j2`. Expanded dependency tests subsequently passed14 cases/90 assertions.
- **Lesson**: enabling a previously disabled target can reveal stale optional dependency paths even while the shared-library target builds successfully.

## 2026-09-05 — late NAC content callback survives cache invalidation
- **Area**: Spec179 T019, local NAC-ABE Consumer and OpenABE error boundary
- **Symptom**: WAL campaign retry scenario passes all14 business checks, but Provider/A aborts(-6) with `Specified length is invalid` and uncaught `oabe::_OpenABE_ERROR` immediately after epoch3 installation. The process-exit gate correctly rejects it. Driver stopped(exit143); six completed probes retained, five passed. This is incomplete failed evidence.
- **Mechanism reproduced**: Consumer increments `m_cacheGeneration` only in `clearCache`; neither asynchronous content nor CK completion/error checked it. `nac-consumer-revocation-red2.log` fails5/23 assertions: late content reaches crypto with cleared DKEY and produces the same OpenABE error; late CK refills the cache. CP/KP enum conversion fails2/4 separately. GDB on the preserved old binary/library catches the enum in `constructKeyFromBytes -> parseKeyHeader -> importUserKey -> ABESupport::decrypt`, with the caller blocked in `Consumer::onCkeyData -> decryptContent -> SegmentFetcher` (`gates/nac-late-content-gdb-red.log`). This is the controlled reproduction's stack; the original network process has no stack dump.
- **Fix**: generation-fence both fetch stages' completion/error callbacks and normalize OpenABE enum errors into `NacAlgoError`. Unchanged Consumer tests pass23/23 and CP/KP error/recovery passes4/4; expanded dependency14 cases/90 assertions pass. Committed as NAC-ABE `8b462d0`, exact prefix installed and NDNSF resolution verified. Final NDNSF unit182/182, integration72/72 and full MiniNDN16/16 pass; the retry scenario retains14/14 business checks and all five processes exit0. No wire format, permission ownership, or revocation timing changes.
- **Closure evidence**: `specs/179-request-scoped-confidentiality/evidence/online-authorization-audit-20260905.md` and `results/spec179-online-auth-20260905/campaign-nac-final/`. This final cohort also closes the earlier pending online-grant, initial-DKEY admission, callback, namespace fault and PIB regression entries below; their intermediate failures remain historical evidence.
- **Lesson**: clearing maps and rejecting new requests does not cancel callbacks already admitted under old authority; every delayed stage must retain and verify its originating generation.
## 2026-09-05 — shared MiniNDN PIB reader races another role's startup write
- **Area**: Spec179 online-grant fixture, shared campaign PIB
- **Symptom**: the final-readiness late-grant probe had target21/21 but control0/0: User/A exited1 with `Signing certificate ... does not exist` before its first request. The gate correctly failed. Its certificate/key remained present in the retained PIB. Ordinary grant and eight other completed probes are retained; the driver was stopped (exit143) after this failure, allowing the active ninth probe to clean up normally. This is an incomplete failed campaign, not16-scenario acceptance.
- **Root cause evidence**: the fixture used DELETE journals; installed ndn-cxx PIB SELECT paths treat non-ROW, including BUSY, as absent and configure no busy timeout. The added concurrent-reader regression reproduces `database is locked` under the old setup (`gates/pib-concurrency-red.log`). Lock contention is the supported explanation for the transient App failure; that process did not log SQLite's nested return code.
- **Fix**: initialize the campaign-only PIB in WAL mode before any role starts, require the returned mode to be WAL, and record it in the manifest. Existing writer initialization serialization remains. No host PIB, native library, permission rule or deadline changes. Full harness and a fresh network campaign verify the repair.
- **Lesson**: isolated network namespaces do not isolate an intentionally shared SQLite key store; preserve concurrent signing reads as well as serializing initialization writers.

## 2026-09-05 — base RequestMessage overload bypasses shared admission helper
- **Area**: Spec179 initial-DKEY repair verification
- **Symptom**: first repaired build passed the unwrap callback checks but still failed the two publication checks (6/8, `gates/readiness-green.log`).
- **Root cause**: five convenience/Targeted callers used `prepareRequestControllerVersion`, but the base RequestMessage overload entered `startRequestServiceWithRequestId` directly, duplicating only status/version checks. Checking the helper's callers alone missed that separate entry.
- **Fix**: replace the duplicated base-entry checks with the same shared readiness/status helper. The unchanged regression now passes8/8 (`gates/readiness-base-green.log`); all five native targets rebuilt successfully with `-j2` in12m58.644s (`gates/build-base-admission-green.log`). Expanded unit182/182 and integration72/72 passed (`gates/unit-base-final.log`, `gates/integration-base-final.log`); MiniNDN remains pending.
- **Lesson**: trace from the public failing call to the publication boundary; a helper's caller list does not prove every entry uses it.

## 2026-09-05 — permission renewal admits requests before initial DKEY installs
- **Area**: Spec179 asynchronous User startup and hybrid decrypt callbacks
- **Symptom**: normal grant still failed with target11/13 despite status refresh. NAC Consumer debug shows permission renewal at12s coalescing behind the initial DKEY fetch; the stale result is discarded near15s before a replacement installs. ACKs for the first two requests arrive while the Consumer reports no private decryption key, with no application error callback.
- **Root cause**: removing constructor blocking exposed an implicit prerequisite: Request admission checks permission/status but not initial Consumer readiness. Both runtime hybrid decrypt functions move `onError` into the success closure before constructing the unwrap error callback, leaving the latter empty.
- **Fix**: gate real network Request admission on initial Consumer readiness while retaining LocalMock fixture semantics; copy error callbacks into the asynchronous success/unwrap branches and retain synchronous exception reporting. The real-constructor regression reproduced four failed assertions (publication1 instead of0, errors0 instead of1); `gates/readiness-red.log`, exit201. The pre-fix binary is retained. Fixed rebuild/regression is pending.
- **Evidence**: `grant-crypto-diagnostic/user-B.log`; earlier `campaign-grant-final` also retains one control timeout at the grant/status transition. Exact-version rejection is expected during distributed convergence; do not claim instantaneous default-policy convergence or hide that failed row.
- **Lesson**: asynchronous construction must replace former implicit prerequisites with explicit admission checks; moving a shared callback into one branch can silently disable another branch.
- **Probe setting**: normal grant now uses the existing 250ms status refresh knob (four opportunities per 1rps request interval); late grant retains1s. Every failed row remains counted. These measured settings do not establish zero interruption with default status-refresh timing.

## 2026-09-05 — grant control failures were not included in the gate
- **Area**: Spec179 grant-only control and status-convergence evidence
- **Symptom**: normal renewal probe failed with target12/13 and control12/24 successes. The collector exposed control successes only, so the twelve control failures were absent from gate conditions.
- **Root cause**: successful-only control projection lacked a companion failure count. Separately, this grant probe disabled scheduled status refresh: providers learned epoch2 from target traffic while User/A remained epoch1, and the first target request's ACK waited on Provider refresh. Exact-version rejection remains required; a short convergence probe cannot assume all peers instantly discover grant-only changes.
- **Fix**: record every control row and require zero control failures; the new regression fails before the change. Use the existing 1s status refresh knob for the normal grant probe, matching the corrected late-grant case and other bounded revocation tests. This is an explicit experiment/deployment setting, not a claim of instantaneous convergence under production defaults. Retain the 12/24 negative run; recheck earlier late-grant raw rows against the stronger control rule. Final normal grant rerun pending.
- **Ref**: `campaign-renewal/grant-only-advance`; `gates/harness-control-red.log`; T018.
- **Lesson**: key reuse and status-version convergence are separate conditions; count all control outcomes, not only successful ones.

## 2026-09-05 — asynchronous startup exposes three MiniNDN timing assumptions
- **Area**: Spec179 grant and offline-rejoin probes (T018)
- **Symptom**: rebuilt full campaign finished 13/16, exit 1. Normal grant had no renewal and zero granted requests; late grant had 21/21 successes but no exhausted permission retries; offline User installed epoch 2 before reaching epoch 3.
- **Root cause**: old constructor blocking implicitly deferred permission discovery until grant. Once constructors return, an empty permission response completes normally, so absence of a grant does not force transport retry exhaustion. Fixed SIGCONT timing relied on old startup skew and could precede the actual epoch-3 revoke. The late control also needed scheduled status renewal for its 60-second window.
- **Fix**: both grant probes use explicit App refetch; late probe drops outgoing UDP only inside the isolated user-b namespace until observed final permission timeout, then removes the exact rule in finally. A namespace guard refuses host execution. Keep control statuses renewed through existing knobs. Offline SIGCONT waits for the actual revoke marker. No failed rows or ordering checks are removed. Three focused MiniNDN reruns pending.
- **Ref**: `results/spec179-online-auth-20260905/campaign-final/`; `permission-startup-loss.json` in the corrected late run records both fault boundaries. Native binaries and libraries are unchanged for the rerun.
- **Lesson**: test prerequisites must be observed, not inferred from constructor latency, empty responses, or estimated mutation deadlines.

## 2026-09-05 — stream retry timing assertion fails alongside compilation
- **Area**: Spec179 final integration verification / shared-host load
- **Symptom**: `NormalStreamRetriesOneSuppressedEventFromProviderIms` completed successfully but recorded two retries instead of exactly one; the expanded gate returned 201 (70/71 cases, 1269/1270 assertions).
- **Root cause**: the test uses a 100ms Interest lifetime; concurrent `-j2` App compilation is a plausible scheduling cause, not yet established by a controlled load experiment. No source change to the stream implementation occurred in this repair.
- **Fix**: retained `gates/integration-final.log`, finished compilation and reran without compilation load. The isolated case passed immediately (12/12 assertions, exit 0); full isolated integration gate passed 71/71 cases and 1270/1270 assertions (`gates/integration-final-isolated.log`). No retry assertion or timeout was weakened.
- **Lesson**: independent binaries avoid link races but do not isolate timing-sensitive tests from shared CPU pressure. Run the final timing gate without compilation.

## 2026-09-05 — unprovisioned runtime cannot reach online permission renewal
- **Area**: Spec179 User/Provider construction and online grant (T018)
- **Symptom**: late-grant MiniNDN probe failed the App_User readiness deadline; no permission fetch or App refetch marker appeared, only repeated Waiting for decryption key lines. The original first-grant scenario began the App only after grant unlocked construction.
- **Root cause**: both real constructors synchronously pump their Face until Consumer has a DKEY. An identity with no grant cannot finish construction to call the App-owned permission API. This corrects the earlier startup-delay hypothesis: the relevant delay was the DKEY gate, not slow RSA initialization.
- **Fix**: begin the existing asynchronous Consumer fetch and return with an explicit bootstrap-pending marker. Keep all permission/status/key checks. Timed real User/Provider constructor coverage passes 8/8 assertions with zero unauthorized publication/execution; final late-grant network rerun remains pending.
- **Ref**: campaign/grant-after-permission-exhaustion; ServiceUser/ServiceProvider constructors; UnprovisionedRuntimesConstructAndRemainUnauthorized.
- **Lesson**: an application-owned recovery API is unusable if construction blocks waiting for the condition that API must recover.

## 2026-09-05 — one-second benchmark drain truncates delayed valid responses
- **Area**: Spec179 MiniNDN workload shutdown
- **Symptom**: corrected-duration inflight-revocation run reported two unsuccessful unaffected-user requests near workload end, despite ten successful post-revoke requests.
- **Root cause**: the launcher allowed only one second of drain for a three-second Provider delay and five-second request timeout. Open-loop finalization emitted incomplete rows before valid in-flight work could finish.
- **Fix**: drain six seconds (five-second request timeout plus margin), use at least 35-second role windows for the standard scenarios, retain all failures and rerun. No success filter is added.
- **Ref**: campaign/inflight-revocation; App_User drainDeadline; T018.
- **Lesson**: measured-window completion and process survival must include the entire request drain budget.

## 2026-09-05 — failed withdrawal bypassed by grant or same-target retry
- **Area**: Spec179 Controller pending ABE rotation
- **Symptom**: the new real Controller regression produced 10 failed assertions: same-target retry left the old ABE pair; grant removed the revocation during injected rotation failure; direct recovery produced an equal-version conflicting status rejected by RevocationState.
- **Root cause**: duplicate-target return preceded reconciliation; grant never checked pending rotation; reconciliation reused a ControllerVersion whose old parameter identity could already be published.
- **Fix**: reconcile before duplicate handling and grant mutation, persist a newer epoch before recovery crypto work, return successful completion for the pending same-target retry, and retain ordinary completed-duplicate no-op behavior. One-shot injection and a repeated App revoke support the matching MiniNDN scenario.
- **Ref**: T017; PendingRotationFencesGrantAndPreservesImmutableStatus; `results/spec179-online-auth-20260905/gates/controller-red.log` (exit 201, 10 failed assertions) and `controller-green.log` (exit 0, 36/36 assertions, including old/replacement DKEY decryption). MiniNDN `campaign/revocation-rotation-failure-retry`: 14/14 checks pass, epoch 2 -> 3, affected denial throughout, 16/16 unaffected post-recovery calls succeed.
- **Lesson**: failure recovery is an authorization mutation too; test every public entry and preserve already published immutable status identities.

## 2026-09-05 — late-grant probe initially missed its timing contract
- **Area**: Spec179 MiniNDN T018 probe
- **Symptom**: first late-grant run completed but gate failed (exit 4): user/B started after the Controller grant, no startup timeout exhaustion occurred, and the result exporter omitted the new scenario's grant evidence.
- **Root cause**: startup outlasted the 12-second grant offset; the later probe identified the constructor DKEY wait (see the entry above), superseding the initial RSA-delay hypothesis. One scenario-name equality remained in the evidence return despite sharing the grant collector.
- **Fix**: grant evidence now follows the grantOnlyAdvance configuration for both scenarios; added an exporter regression. Increased grant/renewal/workload windows and require measured exhaustion < grant < refetch <= first invocation plus post-refetch unaffected successes. First run retained as a failed timing probe.
- **Ref**: results/spec179-online-auth-20260905/late-grant-first; 12 launcher tests pass. Corrected network rerun pending.
- **Lesson**: launch offsets are assumptions; acceptance must verify event ordering from observed timestamps.

## 2026-09-05 — MiniNDN open-loop milliseconds interpreted as seconds
- **Area**: Spec179 launcher workload lifetime
- **Symptom**: a run configured for 16 seconds kept enqueueing until the process lifetime killed it; late-grant-first user/A enqueued for about 51 seconds despite the nominal workload/count.
- **Root cause**: requestDurationMs was passed directly to App_User --duration, which uses std::chrono::seconds; --count applies to closed-loop mode and does not cap this open-loop workload. Teardown could therefore truncate in-flight requests, previously hidden by the grant collector.
- **Fix**: round milliseconds up to integer seconds at the App_User command boundary. The final late-grant probe uses 60-second workloads for both users, spanning explicit renewal at 40 seconds after App startup; the fault/retry scenario explicitly spans both mutation events and drains before process shutdown.
- **Ref**: examples/App_User.cpp openLoopDurationSeconds and measurementStopAt; run_request_scoped_confidentiality.py user_command; T018.
- **Lesson**: verify units at the actual CLI consumer and distinguish open-loop duration from closed-loop count.

## 2026-09-05 — online authorization audit detects censored MiniNDN failures
- **Area**: Spec179 MiniNDN evidence and exit status
- **Symptom**: a success plus a failed request while providers were alive was counted as one successful row; a completed run with gatePassed=false returned exit code 0.
- **Root cause**: the grant collector used the earliest provider log timestamp as a termination cutoff and dropped failure rows; main checked process completion alone.
- **Fix**: retain all terminal rows, allow bootstrap/workload/drain in the grant scenario lifetime, and require gatePassed=true for exit 0. Two regression cases reproduced both defects before the fix; the 11-case launcher suite passed afterward.
- **Ref**: tests/minindn/test_request_scoped_confidentiality.py; /tmp/spec179-online-auth-harness-red.log and harness-green.log; T018.
- **Lesson**: process completion is not a security gate; never infer teardown from a first log or silently discard negative evidence.
- **Real-run confirmation**: reprocessing `results/spec179-online-auth-20260905/baseline-grant` retained 16 requests with 14 successes and 2 timeouts. The old collector had reported 14/14. The campaign driver now propagates failed gates, uses fresh output directories, and refuses to overwrite retained scenarios; each new manifest records revision, working diff and executable/library hashes.

## 2026-09-05 — online authorization audit preflight and test authoring corrections
- **Area**: Context Mode and Controller regression fixture
- **Symptom**: active authority hashes were stale; project query guard rejected a low-entropy identifier and then an identifier absent from its query; a new C++ regression did not compile.
- **Root cause**: prior Spec edits were not indexed; malformed guard arguments; makeServiceRevocation takes const char* rather than std::string.
- **Fix**: reindexed canonical authority documents, verified active health, corrected and reran the guarded project query; passed the temporary URI through c_str for the immediate copying helper call.
- **Lesson**: use file-backed checkpoints after retrieval failures and verify fixture signatures before writing a regression. An initially rejected query is not accepted authority.

Append-only engineering failure record. Rule (AGENTS.md): every failure that
costs non-trivial debugging MUST be appended here **in the same checkpoint
commit that fixes or records it**. New tasks MUST read the recent entries as
part of task context. Format per entry:

```text
## <date> — <one-line symptom>
- **Area**: <spec or module>
- **Symptom**: <what was observed>
- **Root cause**: <why>
- **Fix**: <what changed / workaround>
- **Ref**: <commit, evidence file, or script>
- **Lesson**: <one line to carry forward>
```

## 2026-09-05 — git index duplicate entries wrote a corrupted tree
- **Area**: tooling/git
- **Symptom**: `git add -A` with a pathspec containing a comma staged
  duplicate index entries; the resulting commit tree had `duplicateEntries`
  + `treeNotSorted` (`git fsck` errors), and a rename-detection warning
  "duplicate destination".
- **Root cause**: comma pathspec left the index with unordered/duplicate
  stage entries.
- **Fix**: `rm .git/index && git reset --mixed <last-good>` rebuilt the
  index; re-staged with explicit paths; `git prune --expire=now` dropped
  the bad commit.
- **Ref**: NDNSF commits `32b1fc23` (re-created) replacing the bad
  `8fc879ce`.
- **Lesson**: never use `git add -A` with comma/odd pathspecs; verify
  `git ls-files | sort | uniq -d` is empty before committing.

## 2026-09-05 — stale campaign-summary.tsv contradicted final MiniNDN results
- **Area**: Spec179 evidence
- **Symptom**: `results/spec179-minindn/campaign-summary.tsv` showed many
  scenarios `gatePassed=False` while per-scenario `result.json` said true.
- **Root cause**: the TSV predated the final campaign runs (13:08 vs runs
  15:47–17:18) and was never regenerated.
- **Fix**: regenerated from the final `result.json` files (14/14
  `gatePassed=True`).
- **Ref**: Spec179 `evidence/post-implementation-audit.md` R179-A4.
- **Lesson**: derived summary artifacts must carry a timestamp and be
  regenerated, or deleted, after the runs they summarize.

## 2026-09-04 — MiniNDN campaign caught two admission defects
- **Area**: Spec179 runtime
- **Symptom**: S9 — `RequestServiceTargeted` issued versionless requests;
  S10 — `requestServiceStreamingBytes` discarded the admission result so a
  denied stream start logged STARTED.
- **Root cause**: missing version binding on the Targeted request path;
  ignored revocation admission result on the stream start path.
- **Fix**: fixed in `ServiceUser.cpp`; rebuilt; genuine campaign rerun
  green.
- **Ref**: `evidence/minindn-campaign-20260904.md`, NDNSF commit `e7ea0a74`.
- **Lesson**: the cross-process campaign is the admission-boundary oracle;
  component tests did not catch either defect.

## 2026-09-03 — OpenABE mixed-generation decrypt returns garbage
- **Area**: NAC-ABE/OpenABE crypto
- **Symptom**: decrypting new-generation ciphertext with a retained old
  DKEY could return garbage plaintext instead of throwing.
- **Root cause**: OpenABE generation mismatch does not always fail loudly.
- **Fix**: Spec179 test assertions use `decryptFailsClosed` (throw **or**
  recovery failure both accepted); RV-U20 mixed-generation matrix with
  fresh ciphertext per case (the ABESupport singleton CK cache would
  otherwise mask the mismatch).
- **Ref**: Spec179 `evidence/runtime-revocation-lifecycle-20260904.md`.
- **Lesson**: crypto-negative assertions must accept "recovered garbage"
  as failure; never reuse a successfully-decrypted ciphertext in a
  generation-mismatch case.

## 2026-09-03 — NAC-ABE stale DKEY after grant-only policy replacement
- **Area**: NAC-ABE dependency
- **Symptom**: target refresh could receive the previous complete DKEY.
- **Root cause**: DKEY segments published with `FreshnessPeriod=4s`; the
  unversioned `MustBeFresh` discovery Interest then hit a still-fresh
  Content Store copy of the old policy.
- **Fix**: DKEY segments now publish with `FreshnessPeriod=0`; exact
  versioned segment names remain retrievable.
- **Ref**: NAC-ABE `Experimental` branch commit `b1c9c4f` (not pushed).
- **Lesson**: any in-place policy replacement needs freshness discipline
  on unversioned discovery names.

## 2026-09-03 — versioned exact public-params Interest could never match
- **Area**: NAC-ABE dependency
- **Symptom**: after status installation,
  `refreshPublicParameters` with the exact
  `/PUBLIC-PARAMS/<ABE-TYPE>/v=<version>` name (CanBePrefix=false) timed
  out repeatedly.
- **Root cause**: `AttributeAuthority::onPublicParamsRequest`
  unconditionally appended `<ABE-TYPE>` + version to the Interest name,
  producing a Data name that can never satisfy the exact request.
- **Fix**: detect an already-versioned name and do not append again;
  ParamFetcher binds expected name/digest.
- **Ref**: NAC-ABE `Experimental` commit `b1c9c4f`.
- **Lesson**: producer-side name derivation must mirror every Interest
  shape the consumer may legally send.

## 2026-09-02/04 — build and test-environment traps (Spec179 baseline)
- **Area**: build/tests
- **Symptom** (three independent traps):
  1. GCC 9 ICE on `data-enc-dec.cpp` — NAC-ABE must be built with
     `clang++-10`.
  2. Two concurrent waf builds in different out dirs conflict on the
     shared lock and one is killed silently.
  3. After a full-suite SIGSEGV, Boost.Test keeps running and the
     residual process disturbs later timing runs — kill residuals before
     re-running.
- **Fix**: documented build recipe (clang++-10, single build at a time,
     kill-then-retest).
- **Ref**: Spec179 `evidence/restore-fixes-20260904.md`,
     `evidence/regression-red-green-20260904.md`.
- **Lesson**: environment traps must be recorded next to the build
  recipe, not rediscovered per session.

## 2026-09-02 — DummyClientFace hangs and LocalMock DKEY reattach
- **Area**: tests
- **Symptom**: `processEvents` blocked forever on a fully idle face;
  pump-driven LocalMock members could not verify DKEY segments.
- **Root cause**: deferred DKEY reattach had no bound when the face went
  idle.
- **Fix**: bounded retry (250 ms × 20) for deferred DKEY reattach;
  request-pump fixture extended to pump the AA face (Spec179 remounts).
- **Ref**: Spec179 baseline fixes in NDNSF commit `e7ea0a74`.
- **Lesson**: every deferred async retry needs a bounded schedule or an
  idle-face test can deadlock the whole suite.

## 2026-09-02 — SegmentFetcher infinite fetch on discovery Data
- **Area**: Core `ServiceProvider::replyFromIMS`
- **Symptom**: SegmentFetcher kept requesting segments until timeout.
- **Root cause**: discovery Data served from IMS lacked `FinalBlockId`.
- **Fix**: forward to the last contiguous IMS segment and set
  `FinalBlockId`.
- **Ref**: Spec179 baseline fixes.
- **Lesson**: any segmented Data served to a SegmentFetcher must carry a
  terminal marker or the fetch is unbounded.

## 2026-09-07 — Spec182 T006-D: worker child catch mislabels every chain rejection as DI_NATIVE_ONNX_WORKER_INTERNAL
- **Area**: spec182 T006-C/T006-D native worker wire protocol (frozen in
  T006-C); child side of `runNativeOnnxAssemblyWorkerMain`.
- **Symptom**: focused `Spec182OnnxActivation` case
  `ActivationRejectsCertifiedGraphPoisonThroughWorker` failed while T006-D
  verified the real worker binary: the frozen `reject-identity-digest`
  vector (poisoned `recipe.graphDigest`) surfaced from the parent transport
  as `DI_NATIVE_ONNX_WORKER_INTERNAL` instead of the required chain
  rejection code `DI_NATIVE_ONNX_RECIPE`.
- **Root cause**: off-by-one in the child catch of
  `NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.cpp`
  (shipped in the T006-C commit). The guard was
  `what.compare(0, 14, "DI_NATIVE_ONNX_") == 0`, but the family literal is
  15 bytes; the three-argument `compare()` treats the whole literal as the
  right side, so it compared a 14-byte prefix of `what` against the whole
  15-byte literal — never equal. Every chain rejection (`fail(code)` →
  `"DI_NATIVE_ONNX_" + code`, S1-S7 of the certified chain) was therefore
  reclassified as `DI_NATIVE_ONNX_WORKER_INTERNAL`, and the parent
  transport (which propagates the worker error code verbatim when it
  carries the `DI_NATIVE_ONNX_` family prefix) relayed the wrong reason.
  The frozen suites had never pushed a reject row through the real worker
  child, so the defect only surfaced during T006-D activation coverage.
- **Fix**: bound 14 → 15 (`what.compare(0, 15, "DI_NATIVE_ONNX_") == 0`),
  restoring verbatim family-code propagation. A new frozen-lock case
  `Spec182OnnxWorkerProtocol/SubprocessChainRejectionPropagatesItsOwnCode`
  spawns the real worker with the frozen reject row and asserts the parent
  sees exactly `DI_NATIVE_ONNX_RECIPE`; registered in
  `tests/fixtures/spec182/case-manifest.json`.
- **Ref**: run dirs retained under `.codex-tmp/` (`t006d-*` logs);
  manifests `tests/fixtures/spec182/case-manifest.json`.
- **Lesson**: a protocol invariant ("reason-family code must arrive
  verbatim at the caller") needs at least one end-to-end lock that drives
  a real subprocess with a frozen reject vector; unit mocks of the child
  catch could not expose the string-compare bug.

## 2026-09-07 — Spec182 T007-A: wscript helper insertion broke _pin_compiler_toolchain; boost 1.71 cannot print std::vector
- **Area**: spec182 T007-A static Rust tokenizer link; root `wscript` and
  `tests/unit-tests/di-native-tokenizer.t.cpp`.
- **Symptom**: (1) configure failed rc=2 with `NameError: name 'tools' is
  not defined` at wscript ~line 152 — the `_ensure_tokenizer_bridge` helper
  had been inserted in the middle of `_pin_compiler_toolchain`, and that
  function's tail statements (`conf.env.NDNSF_LINKER`, the 'Closed C++
  toolchain' msg) dangled at 4-space indent, becoming the helper's last
  statements. (2) The compile fix then revealed a duplicate
  `_pin_compiler_toolchain` def (original tailless copy plus a reconstructed
  complete copy) — Python shadowing made it work but left ~25 lines of dead
  code. (3) `unit-tests` compile failed on `BOOST_REQUIRE_EQUAL(encode(), ids)`:
  boost 1.71's `print_helper` has no `operator<<` for `std::vector<long>`.
- **Root cause**: (1) `Edit` with an old_string ending mid-function appended
  the new helper inside the old function body; indentation kept the tail
  inside the helper. (3) boost 1.71 test-tools cannot stream a vector; the
  assertion is fine at runtime but does not compile.
- **Fix**: (1) re-emitted the helper as a complete module-level function and
  restored `_pin_compiler_toolchain` with its own tail; (2) deleted the dead
  original copy, keeping one documented def; configure rc=0 with 'Pinned
  Rust tokenizer staticlib' resolved. (3) switched vector equality to
  elementwise `BOOST_REQUIRE_EQUAL_COLLECTIONS` (three sites:
  `compareVectorCase`, owner-reuse roundtrip, concurrency baseline check).
- **Ref**: run dirs retained under `.codex-tmp/` (`t007-configure-r2.log`,
  `t007-build-r3.log`, `t007-full-regression.log`); cargo PATH lesson: the
  pinned rustc must be on PATH (`rust-prefix/bin`) or `cargo build` dies
  with "could not execute process `rustc -vV`".
- **Addendum（同卡）**: an unfiltered full run (`./build-nac182/unit-tests`,
  no exclusion) segfaulted rc=139 inside the known environment-dependent
  stream-facade family (`PredictiveProviderExactWireValidationAndAtomicFlush`,
  stream-facade.t.cpp:270 last checkpoint); negative preserved, same family
  every spec182 card excludes since T006-B/C/D. The card regression gate ran
  with `--run_test='!StreamFacade'` → 859 cases, No errors detected.
- **Lesson**: insert a new top-level `def` only with an old_string that ends
  at a module-level boundary (blank-line pair); after any structural waf
  edit, re-run configure before building. Assert `std::vector` equality in
  boost 1.71 with `EQUAL_COLLECTIONS`, never `REQUIRE_EQUAL`.

## 2026-09-07 — Spec182 T007-B: stale Rust staticlib silently kept old ABI; python codec vs Rust-std surrogate prefix divergence
- **Area**: spec182 T007-B stable-prefix decode; `wscript`
  `_ensure_tokenizer_bridge`, `tests/unit-tests/distributed-inference-tokenizer.t.cpp`.
- **Symptom**: after rewriting `tokenizer-bridge/src/lib.rs`, the focused
  `Spec182TokenizerStable/*` run still failed with the *old* behavior
  (ByteLevel prefix `[a != a�]`, reject-Fuse stable calls not throwing).
  A reconfigure rebuilt the archive (`13:03:24`) but `./waf build` finished
  in 17 s having relinked nothing: both `libndnsf-distributed-inference.so`
  (12:59) and `unit-tests` (13:01) predated the new archive, and a second
  failure signature then appeared (surrogate cut `[�� != ]`).
- **Root cause**: (1) the archive lives *outside* the build dir
  (`.codex-tmp/spec182-t001-dependencies/tokenizer-bridge-target/...`);
  waf links it by path and does not signature-track external STLIB files, so
  a changed archive alone never dirties the link task — and waf is
  content-hash based, so `touch`ing a source does not help either.
  (2) Authoring-proxy divergence: the frozen ByteLevel row expectations came
  from python's incremental UTF-8 codec (`errors=replace`), which *defers*
  a 3-byte-lead decision until its third byte; Rust std rejects `ED A0`
  eagerly (second byte must be 80..9F), so the surrogate row's mid-prefix
  cuts diverged (`""` vs `"��"`).
- **Fix**: (1) delete `build-nac182/libndnsf-distributed-inference.so` and
  `build-nac182/unit-tests`, then rebuild — outputs missing forces the link
  task to rerun against the new archive (verify with
  `ls --time-style` after every T007-class lib.rs change). (2) rewrote
  `author-stable-vectors.py`'s per-cut model as an explicit mirror of
  `std::str::from_utf8` error attribution (tight E0/ED/F0/F4 second-byte
  ranges, continuation consumption, trailing-incomplete `None`); HF decode
  stays the independent cross-check at full length for every row.
- **Ref**: ABI probes retained at `/tmp/t007b-abi-probe/` (probe.cpp,
  probe2.cpp) and `/tmp/t007b-surrogate-check/` (std-semantics micro
  checks); frozen vectors regenerated, whole-file sha256
  `a80597b3c96833a61a4dd22606b014715f62e2111e0bd7b3325cc97665bc6254`;
  fixture tokenizer shas unchanged.
- **Lesson**: after any `tokenizer-bridge/src/*.rs` change, delete the
  `build-nac182` `.so`/`unit-tests` link products (or run a second
  configure + build and verify mtimes) before trusting a test run; frozen
  per-prefix expectations for ByteLevel must be authored with Rust-std
  utf-8 semantics, not python codec semantics.
