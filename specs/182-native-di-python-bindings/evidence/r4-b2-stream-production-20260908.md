# R4-B2 Native Stream Production Chain

## Scope and Status

基线e70161b4；**DONE (local requester stream batch)**。对应T010-C与T011-B的生产接线，
沿用[native token stream](../contracts/native-token-stream-design.md#requester-acceptance-and-recovery-resolution)
及既有GenerationRecoveryV1。不重新设计Spec182，不将本批完成等同于T016真实网络资格。

## Final Local Result

实际client/Core callbacks→accept→eligible replacement→重新规划/授权/commit→final的
本地fixture链通过；前缀不回滚、旧attempt不复活、已接受terminal不恢复，final须与接受
token/text一致。生产planner反馈与group operation同源，feedback不加入单轮readiness。
配置从实际options派生；paired decoder及stride兼容边界已同步。

- [stream](../../../.codex-tmp/spec182-r4-b2-r3/stream.log)：7 cases/190 assertions PASS；
  [SDK wire oracle](../../../.codex-tmp/spec182-r4-b2-r3/oracle.log)：2个实际recovery request PASS。
- [options](../../../.codex-tmp/spec182-r4-b2/options.log)：2 cases/21 assertions PASS。
- [affected regression](../../../.codex-tmp/spec182-r4-b2-r2/regression.log)：29 cases/695 assertions PASS。
  合计38 cases/906 assertions，来自上述三个运行，不虚称一次全量PASS。
- 新ABI构建879.773s；fixture修复及补验两次增量构建32.338s/32.730s，均PASS。
  [CLI/loader](../../../.codex-tmp/spec182-r4-b2-r3/cli-record.json)检查路径以实际build/examples输出为准。

CLI help与ldd exit0，实际加载本次build/libndnsf-distributed-inference.so；首个CLI探针
路径错误在进程启动前，修正后通过，无产品重编。首轮两个replacement fixture topology
错误已修，失败原始日志及failure-log保留。没有运行中的构建或测试。
本批未启用-O2，不构成发布配置或性能资格。integration三处caller已改为paired factory，
但未运行；真实Provider重算、完整生成/会话及T016网络/no-Python验收仍待完成。
下一步T011 stable epoch与conversation接线，不重复本批已验证成果；T010/T011父任务不关闭。
提交前design validator exit0/ok=true（754个local links）、case-manifest JSON及diff whitespace
检查PASS，见[r3 design](../../../.codex-tmp/spec182-r4-b2-r3/design.json)。两份并发
native-dependency-design.md/native-generation-design.md修改不纳入本批checkpoint。

## Baseline Production Gaps

- NativeInferenceClient.cpp::beginCoreRequest只传BeginCollaboration普通response/timeout；
  Core ServiceUser.hpp已有streamOptions/onStreamEvent/onStreamComplete/onStreamError参数，
  应直接复用，不新增stream网络层。
- NativeInferenceHandle::Operation已有serial worker、绝对deadline、attempt与单终态锁；
  尚无acceptedTokenIds/acceptedText/acceptedTerminalHint/replacementStarted。
- publishEvent/observe是普通观察机制，异常隔离是现有契约；生成事件的接受回调必须
  明确区分，不能全局改变普通observer异常语义或把observer投递当作业务accept。
- NativeGroupProjectionBuilder::build显式拒绝TOKEN_FEEDBACK；其group operation按epoch
  扩展的机制已经存在。需补feedback endpoint/operation身份，不复制group授权实现。
- 现有Python AutomaticStreamingHandle._accept_event在内存锁内接受token，锁外回调；
  不是持久journal。其prefix摘要为逗号连接十进制token IDs的SHA-256。
  原生实现还须按已接受契约验证textDelta、finishHint和final文本，不照搬旧文本缺口。

## Batch Members

| Member | Write / observable outcome | Static gate |
| --- | --- | --- |
| ST-1 | NativeInferenceClient.hpp/.cpp；同一operation内严格event身份/顺序/摘要/预算校验与原子接受，明确生成回调及普通observe边界 | stale attempt、重复/缺序、callback异常与取消竞争不会回滚或重复接受 |
| ST-2 | NativeRequestPlanner、NativeGroupProjectionBuilder与既有projection owner；generation contract、TOKEN_FEEDBACK端点与capability同源，注册Core stream回调 | 复用sealed identity、operation stride和终端Provider授权，不借环境或另一份库绕过 |
| ST-3 | NativeInferenceClient.cpp；eligible attempt1错误触发一次replacement，保留原deadline和已接受前缀，final逐字段核对 | terminal token后禁止replacement，未接受token不进入recovery，旧attempt晚到事件不改变结果 |
| ST-4 | tests/unit-tests/distributed-inference-stream-recovery.t.cpp及现有生产链fixture；三处真实integration caller按既有契约同步 | 六个具名accept/recovery负例及stable delta拼接等于final；测试调用真实operation而非复制状态机 |

修改接口/字段时同步CD-001的原有owner及case-manifest；普通局部细节不另建第二份契约。
每个member编码后只读静态门，全部接线审查后统一构建和相关C++测试。
client布局/公共API改变时先核对受影响消费者ABI边界，不能直接沿用R4-B1的无ABI结论。
真实integration/MiniNDN执行保留T016；本批编写所需测试，不提前运行正式资格。

## Execution History

r2 fixture修复增量build PASS32.338s；stream 7/7 cases、124 assertions PASS/3.069s。
受影响V3Placement/ClientState/GenerationProjection/Sampling回归29 cases、695 assertions
PASS/3.774s。验收核对补入重复epoch、错prefix digest、错requestId、浮点token类型及
结构化error code断言；新增check-stream-recovery-wire.py对实际两个恢复wire执行既有SDK
严格解析/round-trip与独立prefix摘要，不复制native被测算法。仅测试/离线oracle变化，
静态复查其序号/类型/期望与真实accept分支一致；r3继续增量unit及stream/SDK对照，
不重跑未变化的29case回归。原r1/r2日志保留。

首次build PASS/879.773s，session53863已终态；options 2 cases/21 assertions PASS。
stream 5/7 cases、104/106 assertions PASS；两个replacement用例在Provider B新offer
构造时触发topology Provider mismatch，嵌套topology仍为A，未到新offer签名/替换运行。
原始options.log/stream.log及record保留，failure-log已登记。下一步修fixture并增量
重试失败用例；不重新fresh build，不放宽真实Provider绑定。批次仍PARTIAL。

续观察同一exec session53863仍运行，日志已推进至56/281（epoch/catalog/preparer），
尚无compiler error，不重启构建。新case-manifest/源码路径同步后的design validator
exit0/ok=true，748个local links PASS，原始结果见
[design.json](../../../.codex-tmp/spec182-r4-b2/design.json)。这仍不是C++测试结果。

R4-B2首次构建已启动：`.codex-tmp/spec182-r4-b2/`，configure exit0/5.277s；
build目标unit-tests,DI_NativeRequester，-j2，当前运行中（exec session 53863）。后续先poll
同一handle或核对build-record.json终态，不能因观察超时另起构建。配置/命令/完整输出在
该目录configure-record.json/configure.log/build.log，构建终态由runner写build-record.json。
生成CXXFLAGS为-std=c++17 -B/usr/bin，未含-O2；本次为功能/ABI构建，不比较速度或宣称
性能/发布配置资格，T017仍核对发布配置。vmstat去掉首行后样本si=8/0、so=0/0，
未见持续swap；无竞争构建。SDK AST syntax与git diff whitespace PASS，C++测试NOT_RUN。

批末静态续审：已删除full-only decoder伪造stable回调，三处integration调用方改用同摘要
paired factory；integration执行仍T016。planner按tensor/redistribution总数预留反馈index与
epoch stride，native/过渡SDK解析器一致接受dependency_count<=stride<=2**20，旧值兼容；
新增native parser较大stride正例。正式审阅新增operation接受/原子终态、Core callback线程、
attempt隔离、recovery输入/Provider排除、feedback readiness和group索引，以及测试确实调用
生产client的路径；**READY_FOR_BATCH_TESTS**。静态结果不证明运行PASS，具名重算测试
只覆盖requester恢复契约，真实Provider重算仍由现有epoch及T016承担。
准备新R4-B2构建目录：公共NativeRequestOptions/NativeEncodedRequest ABI已改变，不能复用
旧消费者对象作为新ABI验收。系统g++9.4/ld2.34、Boost1.71和原NAC/SVS/ONNX闭包已核对，
无竞争构建；沿用先前swap降档-j2。原R3/R4-B1二进制/日志保留。

最新ST-4 authored：复用di-native-v3-placement.t.cpp原PublicClient fixture的真实client、
Ed25519 offer admission、grant和Core commit，抽成runPublicClientScenario；旧三个unary
scenario继续注册。新增Spec182StreamAcceptance七case（六个原具名负例及成功完整transcript），
由Core登记的stream callbacks驱动真实operation，非复制状态机。恢复场景检查新Core request
中的GenerationRecoveryV1已接受/未接收前缀，随后提供Provider B的签名ACK并完成final；
保留Provider A迟到event/final，核对公开requestId稳定、callback次数及最终文本。
以上是authoring，尚未编译或运行；不证明Provider实际重算或真实网络资格。此处“重算”
具名case只验证请求恢复前缀和替换路径，真实Provider epoch重算继续由T011/T016证明。
剩余静态项：多tensor operation stride与旧SDK依赖数量校验兼容、legacy full-only decoder
不能冒充stable decoder，以及恢复后的晚到回调/最终清理。先关闭这些再整批构建。

最新ST-3 PARTIAL：新增NativeGenerationRecovery值及原GenerationRecoveryV1 request task wire。
envelope校验attempt2/原输入manifest/前缀/旧plan/失败Provider绑定，前缀摘要由实际IDs派生。
operation保留稳定公开requestId，新增每attempt coreRequestId；eligible Core错误仅限
EventTimeout/EventOutsideRetention/ProviderFailure，要求明确且已选中的失败Provider、
当前attempt1、启用一次replacement、未接受终止token、原deadline仍足够收ACK。
错误回调已调用beginReplacement：从接受前缀构造快照、沿用原prepared input、重新编码
attempt2、取消旧Core/清理scope并调用同一beginCoreRequest；planner排除失败Provider。
ACK/commit/timeout捕获attempt，commit持有plan副本；旧IO失败通过expectedAttempt的
单终态门过滤，不能把attempt2改成失败。新的绝对deadline timer不重建。
只读源核对以上分支并修正旧commit共享optional读取及旧IO失败跨attempt问题；尚未
编译/测试，不授予STATIC_PASS。下一步补六个真实operation/transport fixture用例及
recovery wire独立对照，核查多tensor stride和remaining callback/queue竞争，再统一构建。

最新输入绑定进度：NativeRequestEnvelope的nativeGenerationFromOptions从实际input.options
解析生成参数，复用SDK采样别名/default与canonical digest，拒绝非整数token/预算、错误
采样类型/范围和错generation身份。client自动从TOKEN_STREAMING任务options派生契约，
显式native generation必须逐字段一致；stride由planner后填。planner在ensureArtifacts之前
核对source token input和各role state I/O，避免发布后才发现生成输入不匹配。
新增distributed-inference-stream-recovery.t.cpp的Spec182GenerationOptions两个case，包含
默认摘要独立参考、采样别名和12种错误options。Python标准库独立摘要为
sha256:f924aa62a0ced09ee7e05e43971f95cdae88f7cc116b2dd9bf6ca8d6661d224b；
本轮仅生成该offline reference、检查diff whitespace，C++尚未编译/执行。
下一步仍是ST-3实际replacement与ST-4 operation/Core测试；新options测试不替代六个
stream acceptance/recovery具名用例。所有产品改动保持未提交、TESTS_DEFERRED。

最新ST-2 PARTIAL：planner从单源/单终端pipeline派生唯一TOKEN_FEEDBACK（复用SDK的
input_ids/int64[1,1]布局、scope摘要与命名），冻结stride及sorted-provider group rank到
封存执行计划。group builder不再一概拒绝feedback，单Provider自反馈也分配group；从封存
依赖产生TOKEN_FEEDBACK capability operation并保留其索引，普通operation跳过该保留索引。
feedback继续不进入NativePlanProjectionBuilder的单轮readiness，避免epoch0等待环。
只读源码核对以SDK排除feedback readiness的路径、NativeEpochCoordinator发布边和Core
group验证为依据；修正proposal Provider来源为providerByRole而非不存在的role.provider，
并加强feedback layout/tensor/rank一致性拒绝。尚未编译/测试；生成输入/采样绑定、replacement、
多tensor stride边界及真实C++正负例仍待完成，ST-2不计STATIC_PASS。

最新ST-1接线：NativeRequestOptions复用generation contract/Core StreamRequestOptions，
新增独立native onGenerationEvent回调，symbol-design同步所有权。提交冻结generation身份、
attempt/stream epoch与原deadline，planner传同一generation到sealer。Core三个stream回调
已接operation：event有界入队后接受再调用应用，callback异常保留前缀并失败；complete
核对final并经过单终态门，普通Response不得绕过。取消复用Core已有stream清理。
静态复查补齐final解码后deadline和显式generationId复用。尚未编译/测试，ST-2反馈projection、
generation输入绑定、replacement与真实stream fixture仍待完成，不授予STATIC_PASS。
公共options布局已变，后续构建须检查所有消费者ABI。本轮修改保留未提交。
以下保留上一轮记录，以本段接线状态为准。

ST-1 PARTIAL：NativeInferenceHandle::Operation已加入内存接受前缀及acceptGenerationEvent/
validateGenerationFinal。校验真实GenerationTokenEventV1的整数类型/非负ID/连续epoch/
prefix digest、textDelta/finishHint、可选显式身份、token和文本预算；候选分配完成后swap
提交，final核对完整token/text/hint并拒绝错身份。旧attempt在parse前忽略。
只读review-agent方法已检查整个新增分支及现有epoch生产wire；修正final缺少输入大小界限
和可选身份核对。仍未授予ST-1 STATIC_PASS：Core认证binding来源、事件队列容量和接受回调
尚未接通，也尚无本批C++用例。这些私有方法当前未被生产调用，不计可用stream能力。
下一步完成ST-1调用入口/交付队列，再继续ST-2/ST-3；整批前不运行构建。

CodeGraph定位当前NativeInferenceClient.cpp后以canonical源码核对以上边界；搜索结果中
.codex-tmp staging副本已排除，不作为当前实现依据。本批尚未运行测试。
下一步在ST-1完成显式生成回调接线，再继续ST-2/ST-3；不得以孤立
accept辅助类测试通过代替完整Core回调接线。进度表保持T010-C PARTIAL。

本次批次登记的validate_design.py检查PASS（ok=true、748个local links），git diff
whitespace检查PASS；仅文档/执行边界核对，不构成产品STATIC_PASS或行为验收。
