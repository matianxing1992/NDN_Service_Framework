# Integration Baseline — 2026-09-06

## Final Local Validation

Status: VALIDATED_PENDING_COMMIT。三个真实 MiniNDN 场景均 PASS：user-identity-revocation（49.108 s，12/12 checks）、grant-only-advance（58.985 s，专用 grantOnlyGateOk=true）、provider-identity-revocation（58.295 s，14/14 checks）。最后一项主动 SIGINT 旧 Provider 后重启，旧进程 exit -2，其余 exit0；没有节点进程残留。证据和实际库摘要见 [validation record](merge-validation-20260906.json)。静态设计审查、完整 C++ suites 和 current Python profile 均通过；不宣称历史全量 Python suite、181最终资格或182原生迁移完成。

## Validation History

静态修复后 full unit **759/759 PASS**、full integration **154/154 PASS**；current Python profile **2171 passed / 22 skipped**（248.890 s）。完整历史 Python suite 的旧冻结输入失败不因 current profile PASS 被改写。原始 `unit-static-r2`、`integration-static-r2`、`python-current-r2` 及 [static review](static-review-20260906.md)。

MiniNDN `minindn-user-revocation-r1` **BLOCK at launcher**（exit1、0.644 s）：PATH 未含系统 `/usr/sbin`，Mininet 找不到已安装 `ifconfig`；不是协议结果。增加网络工具目录后在新目录重试，原始 `.codex-tmp/merge-20260906/minindn-user-revocation-r1/output.log` 保留。

## Scope and Status

Status: IN_PROGRESS。用户授权先整合 NAC-ABE / UAV 开发成果，完成合并后的 unit/integration 验证，再修订 Spec182；不启动原生迁移实现。Spec181 的独立最终验收暂停，未完成责任由 Spec182 接收，不宣称 Spec181 PASS。

## Source Boundaries

Tracked workspace preservation check：原 113 个变更路径没有缺失，102 个候选文件与原工作区逐字节相同；其余为合并的 Core/测试/构建文件、失败历史和活动指针。两份 UAV slides 属于既有独立编辑，保留在原工作区，不混入本次代码合并提交。原 failure-log 的未提交补充经 `git apply --check` 后完整纳入双方历史。

NAC-ABE final dependency checkpoint: `5ed23e6`（merge `e205b72` + installed metadata repair），已 fast-forward 回原 NAC 仓库；原 3-file 工作区保留在具名 stash 中，不重新叠加已被远端包含的 patch。

- NDNSF local parent: `d4a5e39ce5b4a023f6e55d2440c60aa998983f8f`。
- NDNSF remote parent: `4391af81cd24ff5510aa52b48ab9cec0fdec1ebb` (`origin/UAV-Experimental`)。
- NAC-ABE local parent: `1cc17d9d21f4dfc0921cc77315d0c57d46291880`。
- NAC-ABE remote parent: `c3aafa6ec5a566879942107c7b20855659c9dfb9` (`origin/Experimental`)。
- 隔离工作区：`/home/tianxing/NDN/ndnsf-integration-182` 和 `/home/tianxing/NDN/nac-abe-integration-182`；原工作区保留。
- 本机 tracked diff 通过三方合并纳入；299 个非冲突源码、配置、文档文件复制进入候选工作区。旧构建、模型、私钥、实验原始输出不纳入源码提交。4 个 Context Mode 同名文件需独立比较。

## Resolution Design

- D2h trace R1 定位 compact segment 的 AAD 恢复错误：`ProviderGroupCoordinator::makeManifest` 用 capability 的 3000/12000 ms 认证预算，`NdnsfCollaborationDependencyIo::fetchBundles` 却从 Selection edge 的 2000/8000 ms 恢复，导致后继 HMAC 拒绝。接收方 inner manifest/descriptor 恢复 capability 原始预算；outer manifest 和 fetch deadline 仍遵守 edge。没有改 HMAC、nonce、签名或截止时间校验；既有 D2h 121/212 oracle cases 是不同预算的真实跨角色回归。

- Python current-fixes R1：7 failed / 123 passed；R2：5 failed / 53 passed。Repo fixture 补 v2 migration diagnostics；stream facade fixture 补 readiness 和输入 identity 字段；V3 trust fixture 的 wall clock 与已注入 verifier clock 一致；build fixture 清理外部 NAC prefix；profile 检查追踪 Spec181 已抽出的 `bindNativeRunnerPreparationContext`。这些均保留生产校验。MVCNN 从交付源代码生成的 ONNX 为 `sha256:83d7a1558e23a2792467e70f6947d869fd4664b27e12c019617e6fc0280332f3`，不等于远端注册的 `14ec...`；本地功能测试使用临时 registry 显式登记这个新 subject，仍运行真实 ONNX/native 数值和错摘要拒绝测试，不改远端 registry 或历史结论。
- Spec180 Y-N-I 新增诊断从裸 `std::cout` 改为既有 `logRuntimeEvidence`，先组装完整记录；修复 Spec175 的并发日志单一 sink 契约，字段及边界语义保留。

- Integration R2 GDB exit 255（185.506 s）：Thread 677 在已销毁 Provider 的 assignment fetch 中访问 `Face::getIoContext()`；独立 targeted-stream case 通过，证明跨 case 遗留 worker。`ServiceProvider::prepareCollaborationAssignmentAsync` 和 scope-key fetch 的 detached threads 改为 `m_fetchPool`（2 workers，有界队列）；析构先设置共享 `m_fetchStopping` 再 join；`fetchAndDecryptLargeData` 三处等待按 20 ms 间隔检查取消，所有捕获 Provider 的 Face/NAC 回调执行前检查同一 token。网络获取逻辑、总 deadline 和认证不变；回归需同时覆盖未派发和已发出 Interest 的销毁，销毁后继续 pump Face，确保无回调且 shutdown 有界。

Python R1 collection 首边界：79 个 import 缺新 worktree 的 `_py_repoclient` native extension；另两个测试依赖本机忽略的 slides generator 及无扩展名 `spec175-gate-prerequisites`。补入既有三个纯脚本（含 generator 的 speaker-notes helper），不复制 slide/model 输出。Repo binding setup 同步显式 SVS pair / NAC prefix / exclusive framework dir 校验，以 exact shared objects 和 RPATH 重建；两套 setup 共享参数化 closure 断言，禁止静默回退旧 build。

Integration R2 的多 Provider generation 首边界为 fixture APPLICATION_INPUT endpoint digest 与现有 endpoint 常量碰撞；新 fixture 使用请求/attempt/role 绑定的独立 SHA-256 identity，不放松生产 digest uniqueness 校验。

Readiness R1 首边界：新 NAC `AttributeAuthority::onPublicParamsRequest` 只返回真实 generation 的固定名称，旧 Controller 向 `PUBPARAMS/readiness/<random>` 探测的做法不再匹配。不得恢复“任意后缀重命名当前参数”的旧 NAC 行为。`ServiceController::waitForReadiness` 改为两个有界步骤：临时 `NDNSF/READINESS/<128-bit random>` challenge 证明两个 Face 经同一本地 NFD 到达 Authority，并签名绑定当前参数名称/摘要；再用 exact current PUBPARAMS name 获取且验证名称、签名和实际 bytes。新增 state.challengeReady；临时 filter/pending handle 随 scope 清理；取消、共同 deadline、7-prefix barrier 不变。测试 forwarder 允许两个明确命名空间，保留所有负例，并增加错误 PUBPARAMS 内容拒绝检查。

Integration fixture alignment：手工 V3 projection 为 ingress 角色显式补 `APPLICATION_INPUT` endpoint；D2b Auxiliary `COMPONENT_SET` 使用规范 0..0 layer interval 和 node cover，保留生产 fail-closed 解析。精确 tensor packet 观察改为 unique Data name/bytes 数量，并断言同名重发 bytes 相同；仍检查每一次实际 Data wire <=8800，避免把发布和按 Interest 回送的同一 Data 算成两个逻辑分段。

Build R3 因新 readiness target 插在 Waf declaration 前部而改变对象编号、触发不必要全量重编译，主动 SIGINT（exit 68）；移到末尾恢复既有 target 编号，R4 使用一致来源的已有 fresh objects 和变更对象。不是测试 PASS/FAIL。GDB 独立 targeted-stream 用例正常退出，跨用例 segfault 尚未关闭，后续完整 suite 仍控制合并验收。

Unit R1 diagnosis：空 tensor 的 overflow 检查只在 dimension 非零时除法，仍检查每个维度边界和最终 payload 长度；既有 empty detection/zero-shape 回归保留。R1 encrypted-input fixture 的 `makeSelectionInputKeyOffer` 改为生产契约的 lowercase digest，保留全部 commit/plaintext/terminal 断言。legacy manifest 回归改为验证原格式全部摘要和签名往返；网络包大小继续由相邻 ContextCompact 回归及生产 exact-data gate 检查（见 Spec181 `contracts/exact-tensor-wire.md`），不改变 legacy wire 或 8800-byte 上限。

Controller readiness standalone 以 `tests/wscript` 的 `service-controller-readiness` target 链接配置中的生产库/依赖，替代手写编译命令；外部 launcher 继续在 DSO 加载之前设置私有 PIB/TPM。新增 policy-status 注册失败案例必须拒绝 ready。

Python binding ABI contract：`pythonWrapper/setup.py::explicit_nac_abe_prefix()` 新增可选 `NDNSF_NAC_ABE_PREFIX` 环境输入，验证该 prefix 的 `include/nac-abe/consumer.hpp` 和 `lib/libnac-abe.so`；显式时 `build_extension()` 将头目录排在 installed pkg-config 路径前、以 exact shared object 链接并添加该 lib RPATH。未配置保留既有 pkg-config 开发行为。`scripts/spec180_native_build.py` 已传递外部环境，构建命令须传入与 Waf 相同 prefix。新增定向测试使用相反顺序的旧 pkg-config 路径验证正确选择，缺 header/library 必须拒绝；无测试替代真正 extension rebuild/loaded-path 验证。

NDNSF build R1 首边界为 `subscribeToProducerWithCatchUp` 调用仍残留远端旧 subscribe 的两个 bool 实参，缺少本机已有 `catchUpPublications` 和 `ndn::time::milliseconds(catchUpAgeMs)`。补齐这两个既有捕获值，保留 prefetch=true、packets=false；不新增策略或改变 API。`.codex-tmp/merge-20260906/build-r1.log` 保留编译证据，R2 继续同一 fresh build tree。

Context Mode 合并保留远端较新的 guard/guide，保留本机 38 项完整 guard 回归为独立 `test_context_mode_guard_local.py`，另保留远端 7 项回归。首次执行 42 PASS / 3 FAIL：本机 Claude fixture 仍用旧 platform `claude` 且无 MCP registry，未到被测缺 hook 边界。按新配置契约修复 fixture（`claude-code` + 有效临时 registry），保留原断言。日志 `.codex-tmp/merge-20260906/context-tests-r1.log`。

- NAC-ABE 远端完整包含本机 3 个文件的线程/生命周期修复；保留远端新增的 scheme/key/parameter-bound CK 缓存、异常规范化和默认生命周期测试角色。候选源码应与远端一致。
- `NDNSFMessages.hpp` 同时保留 `CollaborationArtifactDataNameType` 和 Spec179 request confidentiality TLV。
- `ServiceUser.cpp` 保留本机 SVS ABI 诊断与远端请求级授权/密钥逻辑。
- `ServiceProvider.cpp` 保留本机 `subscribeToProducerWithCatchUp` 和密钥封装修复，以及远端 service-bound 缓存与权限刷新。构建使用实际具有 catch-up API 的本机 NDN-SVS 源码/库。
- `wscript` 同时保留 dependency-prefix 隔离和 `--nac-abe-prefix`，所有新构建绑定新 NAC-ABE 头文件与库。
- `ServiceController::registerInterestHandlers` 新增的 policy-status 注册必须纳入 readiness barrier；`RegistrationState::pending` 从 6 改为 7，失败走共同 error callback，避免第七条注册失败仍报告 ready。
- 架构/失败历史保留双方内容，再按合并后权威修订；不覆盖历史实验结果。

## Validation Plan

### Reproduction Scope

新构建固定系统 GCC/Boost/binutils，最多 `-j2`，显式 NAC install prefix 和 NDN-SVS source/build pair；两份 Python 扩展必须在最终 Core/header 上重建。运行前给 `NDN_CLIENT_PIB`、`NDN_CLIENT_TPM` 分配新目录，`LD_LIBRARY_PATH`/`PYTHONPATH` 指向本次 build/两份扩展；不要加载主工作区旧 `.so`。完整 C++ 门为 `build-merge/unit-tests --report_level=detailed` 和 `build-merge/integration-tests --report_level=detailed`，计数只取最外层 `Test module`。Controller `start()` 另运行 `tests/standalone/run-service-controller-readiness.py build-merge/service-controller-readiness` 及 `--real-nfd`。

当前 Python 兼容性范围为 `tests/python/test_spec17[5-9]_*.py`、`test_spec18[01]_*.py`、`test_uav_*.py`、`test_ndnsf_*.py`，加 `test_streamed_invocation_api.py`、两份 `test_context_mode_guard*.py`、`test_python_wrapper_native_closure.py`。显式排除四个外部/历史 qualification 输入用例：`test_ndnsf_di_candidate_lineage.py`（冻结109证据）、`test_ndnsf_di_runtime_aware_campaign.py`（外部Qwen qualification）、`test_ndnsf_native_tracer_runtime_profile.py`、`test_ndnsf_runtime_doctor.py`（生成Qwen tracer profile）。选择文件清单已完整写入每次 current Python raw log 首行；不能将这个范围称为全历史 Python PASS。设置 `SPEC181_ASSEMBLY_PARITY_BINARY`、`SPEC181_NATIVE_PROVIDER_BINARY` 指向本次构建，`SPEC180_YOLO_CHECKPOINT` 指向明确本地 checkpoint；MVCNN 测试自行生成临时输入并显式登记。

Latest correction: 原先 integration R3 的 90/92 是部分 suite 计数。完整 module 为 R3 **140/154 PASS、14 failed**，R4 **142/154 PASS、12 aborted**；11 项因私有 NFD socket 无监听而 abort，1 项 certificate digest 大小写不一致使撤销未匹配。不得把 connectHandler 的 Boost fatal 当作无害日志。最新 full unit R5 **752/752 PASS**（48.247 s）；D2h focused **2/2 PASS**（32 assertions）；native build R4 PASS。Python current R1 **2158 passed / 8 failed / 22 skipped**，八项 YOLO lazy import 首异常待定位；MiniNDN 尚未执行。用户新增前置静态审查，记录见 `static-review-20260906.md`。

生命周期修复后的 full unit R4 **752/752 PASS**，exit 0，48.039 s；新 `ProviderDestructionCancelsQueuedAndActiveAssignmentFetches` 覆盖未 dispatch、已发 Interest、队列等待和销毁后继续 pump。Build R9 775.579 s、最后源码同步 R10 310.262 s，均 PASS，最多 `-j2`；native binding R3 **PASS**，85.689 s，重新核对变更后的 source/ABI。YOLO regression R1 **19 PASS**，MVCNN tests R3 **13 PASS**；contract fixes R3 **115 PASS / 3 skipped**，没有执行 SIF/Tiger 实验。完整 integration R3 尚 IN_PROGRESS。

Repo extension build R1 **PASS**（62.688 s），新前缀定向测试 **10 PASS**。`repo-identity-r1` 从实际 `/proc/self/maps` 验证 `_py_repoclient`、`_ndnsf`、Core 都来自整合工作区，NAC 为私有 install（SHA-256 `41224ebffeb7ed34a8209de83b942cdc0ad3c08fe9eda55173f046a76d58df48`），SVS 为指定 source/build pair（`9fe2bdc9bf5fe2f9f191dda1f11944a86a348ad2de118a95aa739e2c170844ee`）。这条是 Provider 生命周期修复前的 runtime identity；修复后 Core/header 的 identity 必须重新生成。

Python R1 collection **BLOCK**, exit 2，81 errors，25.86 s；`.codex-tmp/merge-20260906/python-r1/output.log`。真实 NFD R2 **PASS**, exit 0，两个不同 probe Face、同一 Authority Face、HopLimit 1→0、NFD clean exit；原始 `/tmp/ndnsf-controller-readiness-591fiwf_/nfd.log`。Native binding R2 **PASS**, 80.713 s，Controller 修复后的 source/runtime identity 已重新核对。

Full unit R3 **751/751 PASS**，exit 0，44.230 s（Controller 修复后）；receipt `.codex-tmp/merge-20260906/unit-r3/`。真实 NFD R1 两次启动实际成功，但 collector 仍搜索旧 response 名称的尾 `/`，未匹配新 exact challenge Data，判 exit 1；原始 `/tmp/ndnsf-controller-readiness-wcclf6j_/nfd.log` 证明两个独立 probe Face 与 Authority 的 round trip。修 collector 为 exact name token boundary，重新运行，不能把 collector failure 写成协议失败或先记 PASS。

Readiness R2 **13/13 PASS**（含 policy-status 注册拒绝、错 current parameters、缓存/签名/跳数/取消/重启/有界 timeout）。原始 `/tmp/ndnsf-controller-readiness-s4a2nbxi/test.log`。Controller 修复的 build R6 PASS（40.896 s）；fixture 最终去重 JSON 字段的 build R7 PASS（34.909 s）。

Native binding R1 **PASS**，82.690 s，`SPEC180_NATIVE_IDENTITY_OK`；receipt/commands 和 mapping 见 `.codex-tmp/merge-20260906/native-build-r1/` 与 `build-merge/spec180-native-build.json`。D2b focused R2 PASS。Readiness R1 exit 124（40 s 外部上限），PUBPARAMS timeout；原始 `/tmp/ndnsf-controller-readiness-5144adkx/test.log` 与 `.codex-tmp/merge-20260906/readiness-r1/`。新 Controller 修复使旧 source identity 失效，须重建/重新验证后才可交付。

Build R5 **PASS**, exit 0（1m16.698s），所有请求的 Core、unit/integration、App、DI native Provider 与 readiness target 均完成。日志 `.codex-tmp/merge-20260906/build-r5.log`。接下来重建 extension 并核对实际加载路径，继续 integration 与 readiness；本条不等同 integration PASS。

Full unit R2 **751/751 PASS**, exit 0，50.583 s；`.codex-tmp/merge-20260906/unit-r2/{output.log,receipt.json}`。保留三个未配置 generated-plan smoke 的范围限制。Build R4 编译报 D2a fixture 修订时误保留未定义的 `selectionObserved` 测试标志，去掉该无关赋值，实际 handler/runtime/response 断言保留；R5 继续编译。

Build R2 完成核心库、unit/integration 和三个 App，但 `di-native-provider` link 缺 `RuntimeStatusStore` 实现；`examples/wscript` 所有显式编译 ServiceUser 的 native target 同步新增该 source，随后 R3 重建。完整 integration R1 60/92 PASS、32 failed（24 aborted），最终 exit 139；segfault 位于 `TargetedStreamRevocationStopsBeforeBootstrap`，另有旧主工作区 Python wrapper 被 helper 误导入的证据，须先重建绑定并显式固定 PYTHONPATH，再完成新轮全量验收。

Ingress trace R1 明确记录同一 exact Selection name 对应人工和自动两次发布，SVS 得到自动空 assignment 并去重另一条。三个旧单 Provider fixture 改用既有 `setSelectionAssignmentPayloadForRequest`，由生产路径发布唯一 Selection；没有新增测试旁路或改变 Provider 去重规则。

Integration R1 正在完成完整 suite；已暴露 legacy ingress 的空 assignment/错误 role 和 D2b 后续角色未执行。原始 `.codex-tmp/merge-20260906/integration-r1/tests.log` 保留；定向 trace 使用独立目录，核对 callback 中人工 Selection 与 RequestService 自动 Selection 是否竞争，不调整 timeout 掩盖失败。

Full unit R1: 747/751 PASS, 4 failed (including 2 aborted), exit 201。原始 `.codex-tmp/merge-20260906/unit-r1/tests.log`。首次边界：`validateNamedTensor` 对零维度执行 overflow 除法；旧 full manifest size 断言；R1 selected input/assignment 未提交。保留完整失败后分别检查既有 wire 契约和解密状态，再修复与完整重跑。三个 generated-plan smoke 因未设置输入而提前返回，不能算真实模型验证。

NDNSF configure R2 PASS，cache 核对 system Boost 1.71、新 NAC include/lib、显式本机 SVS pair、CPU ORT 可用。源码语法检查 634 Python files PASS；binding-prefix 5 项定向测试 PASS（0.64 s）。这些不是完整 unit/integration 结果，full build 仍在进行。

Installed metadata 检查发现 NAC 的首次 configure 在 GNUInstallDirs 初始化前生成 `.pc`，include/lib 指向 prefix 根；Waf 显式 prefix 已规避，但 Python pkg-config 消费者仍受影响。NAC CMake 调整初始化顺序并独立验证新配置；未修改 runtime，46-case 证据范围保留。

NAC-ABE repaired full suite R2 **46/46 PASS**（CTest 1/1、53.95 s），已安装到独立 prefix；实际 test loader 使用 `build-merge-r3/libnac-abe.so` 与 system Boost 1.71。下一步 NDNSF clean configure/build，不复用旧对象。

NAC-ABE build R3 PASS。full test R1 为 41/46 PASS；5 个 AttributeAuthority fixture 因 DummyClientFace 隐式读取主机默认 KeyChain 而在注册签名前失败。修复为显式使用已有内存 fixture KeyChain，不改变生产协议或断言。见 NAC-ABE `docs/integration-20260906.md`，原始 `/tmp/nac-merge-tests-20260906-r1.log`。Context Mode 两套回归合计 45 PASS（R2，0.28 s）。

NAC-ABE clean CMake build/tests/install → NDNSF clean build（最多 `-j2`）→ full unit → full integration → Python suite / rebuilt extension identity → necessary real-NFD/MiniNDN regressions。旧机器 PASS 不替代当前候选测试。每次失败记录首次边界与新 raw run；未通过的阶段不能记为 PASS。

## Tool Boundary

## Build Preflight R1

## Build Preflight R2

显式 Boost 1.71 已生效；第二次 test link 暴露 GCC 驱动从 PATH 调用 Linuxbrew `ld`，造成 system OpenSSL/dl 的错误依赖解析。日志 `/tmp/nac-merge-build-20260906-r2.log`。下一次固定 `/usr/bin` 编译器、binutils 与系统 CMake；源码没有为环境问题删除测试或降低断言。

NAC-ABE clean compile 已生成库，但 test link 首次失败：CMake 选中失效的 `/usr/local/lib/libboost_unit_test_framework.so.1.82.0`。原始日志 `/tmp/nac-merge-build-20260906-r1.log`。修正为显式 system Boost 1.71 路径后在新 build 目录重试。NDNSF configure R1 正确拒绝尚未安装的新 NAC-ABE prefix（`.codex-tmp/merge-20260906/configure-r1.log`），未使用旧库回退。下一步先完成 NAC-ABE 构建、测试和安装，再重新配置 NDNSF。

Context Mode project health PASS；active health exit 4，原因是 feature pointer 为 182 而 AGENTS 指向 181。本轮采用直接仓库权威，文档修订时同步。CodeGraph 广泛查询混入旧 checkout，已拒绝该结果并改用准确仓库路径。
