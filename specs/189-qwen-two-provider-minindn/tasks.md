# Tasks: Qwen 0.6B Two-Provider MiniNDN Full-Path Validation

**Configure dependency checkpoint**: 2026-09-19 — 新增根 `configure.sh`（缺失 OS 包安装后调用 Waf，NDN 库不安装）和依赖清单汇总，Waf 补 SQLite/OpenSSL/Protobuf/ONNX/ORT 编译链接探针。5 项隔离测试、本机只读 inventory、Bash/ShellCheck、Waf Python 语法及限定 diff 检查通过。见 [边界与用法](../../docs/configure-dependencies.md)。未执行 apt、完整 Waf configure/build 或模型实验；专用 SDK 自动安装仍未完成，产品任务不关闭。既有混合暂存区和 hook 阻塞未解除，变更留在工作树，不混合提交或 push。下一步固定专用 SDK 来源/摘要及安装契约，再做独立 configure 验收。

**Repo-handle design checkpoint**: 2026-09-19 — 用户接受 [高层 R6](../../Design/highlevel-design.md)：不可变材料引用与可更新副本 locator 分开，存储返回 handle，读取保留原名并使用经验证的 hint；DI 保持授权/Selection/placement 约束及来源生命周期。文档链接和限定 diff 检查通过；仅高层目标，未修改源码、运行实验或关闭任务。混合暂存区与既有 hook 阻塞未解除，未混合提交。下一步细化引用契约和真实转发验收。

**Repo-mode design checkpoint**: 2026-09-19 — 用户接受两模式约束，更新 [高层 Repo/DI 设计](../../Design/highlevel-design.md) 与 [设计变更记录](../../Design/spec-design-changes.md)。in-app 不接收外部写入、不承担副本责任，但 DI 可主动读取所分配材料并本地缓存；长期保存显式提交 server，活跃读取由 owner/lease 保护，KV 不自动复制。文档链接和限定 diff 检查通过；无源码/API/实验变化，不关闭产品任务。现有混合暂存区及此前提交 hook 阻塞未解除，本轮不混合提交；下一步细化模式准入与 DI 生命周期验收。

**SIF design checkpoint**: 2026-09-19 — 按用户要求补充 [S1–S3](../../Design/highlevel-design.md#sif-构建与复用约束)：先验证并封存全依赖/SDK base，容器内 Waf 构建 NDNSF 并生成新完整 SIF；依赖身份不变时复用 base，改变时增建/重建并重新验证。已对照 TigerCluster 两层文档与 iTiger 维护技能；高层文档 19 个链接均可解析，S1–S3 及限定 diff 检查通过。仅文档更新，未启动 SIF/MiniNDN/集群运行，不改变当前产品验收状态。索引已有并行源码修改且此前提交钩子阻塞，本轮未混合提交。后续交付按 S1–S3 核对输入与镜像身份。

**Installed-runtime policy checkpoint**: 2026-09-19 — 用户新增 [高层设计 B/M 约束](../../Design/highlevel-design.md#编译安装与-minindn-实验约束)：后续 MiniNDN 必须使用系统安装的库、应用、worker 和被测模块；构建树 selector 仅作构建阶段测试。下一次 MiniNDN 前须核对启动器与实际加载路径，存在构建树依赖时先迁移安装规则。本轮仅文档更新，未构建/安装/运行，未关闭产品任务；历史证据保持原样。

**High-level design checkpoint**: 2026-09-19 — 新增 [四模块高层设计](../../Design/highlevel-design.md)，各模块 500–1000 字，包含用例、当前边界和长期原则；README、设计管理与架构阅读入口已接入原则检查。字数、链接与限定 diff 检查通过。仅文档治理，不修改产品代码、API、冻结目标或实验状态，不关闭任何原任务；下一次实施先映射适用原则再验证生产路径。本地 checkpoint 提交被现有索引全量检查拦截（`development-assistant files or references remain in the Git index`），七文件本轮变更保持精确暂存，未绕过 hook、未 push；其他并行改动未纳入。

**Installer maintenance checkpoint**: 顶层安装入口已修正；12 项隔离测试、ShellCheck、Bash 语法与本机只读依赖检查通过。未运行完整安装或原生构建，不改变本 Spec 的 PARTIAL 状态。见 [installer audit](../../docs/install-stack-audit.md)。

**Status**: IN_PROGRESS
**Input**: [spec.md](spec.md), [plan.md](plan.md), [batch-execution.md](batch-execution.md)
**Rule**: Production C++ → C++ assertions → Python orchestration。每任务编码、fixture/调用方/构建注册完成后冻结静态审查；同批组合通过再统一增量构建测试。静态通过不是完成。

## Current Checkpoint

**Spec190 handoff — PLANNED**（2026-09-22 02:20 -05:00）：用户将后续工作调整为多轮token生成延迟分析与独立Spec规划。
跨request驻留T009-R261及ACK/实时交付/同handle/FINALIZE性能工作由
[Spec190 tasks](../190-multiturn-latency/tasks.md)承接；当前Spec189保持PARTIAL，不重写r260历史结果，
不关闭完整Repo、ABI或其他资格缺口。本轮仅规划，无新产品实现/模型运行。

**r260 Request-scoped runner reuse — CACHE_DIAGNOSTIC_PASS / Spec PARTIAL**（2026-09-22 01:55 -05:00）：
r259每个Provider每轮11/12/12次准备已定位并修复；受控C++ RED复现、静态复审、
增量构建与45 cases/1042 assertions三次GREEN通过。全局安装后真实三轮10/11/11
tokenIds与r259相同，均EOS，后两轮KV恢复，各Provider每轮只准备一次（合计70→6）。
Selection→checkpoint合计241.309→63.011秒，监控总区间599.588→426.593秒；
C++ oracle PASS、cleanup PASS、无残留。未改ACK/收尾预算，单次观察非稳定性能保证。
下一步T009-R261实现已接受的短期跨request驻留与shutdown释放；当前尚未实现。
见[r260设计与性能基线](evidence/b189-r260-runner-reuse.md)。

**r259 Conversation affinity and retention — CACHE_DIAGNOSTIC_PASS / Spec PARTIAL**（2026-09-22 01:22 -05:00）：
已实现既有placement子类偏好、本地journal映射认证、Provider有界retention与receipt
一致性。链接fixture边界修正并复审后构建成功；C++43 cases/882 assertions及placement
六case/147 assertions各三次通过。真实两节点三轮10/11/11token均EOS，后两轮两侧
恢复父KV，prefix23→51→79；安装版C++ oracle PASS，cleanup PASS且无残留。
binding ABI重建、双PDF及完整Repo/同handle验收仍开放，不把Python辅助检查当原生
功能证明。未混合提交或push；下一产品验证为常驻C++ Conversation同handle多轮。
见[r259](evidence/b189-r259-affinity-retention.md)。

**r258 Chained multi-turn KV — PARTIAL**（2026-09-22）：限定五目标构建通过，
C++24 cases/638 assertions三次、CLI89例、Python71项通过；真实首轮10token/EOS，
第二轮两Provider复用KV后11token/EOS。第三轮仍选原节点，但父checkpoint已过期，
在restore前失败；cleanup PASS且无残留。下一步修正保留期限契约并实现策略层原
placement优先；当前只有选后映射一致性校验，不能称已实现优先选择或三轮PASS。
见 [r258](evidence/b189-r258-multiturn-kv.md)。保持完整Repo资格、同handle和独立repeat开放。

**r257 Core assignment repair — CACHE_DIAGNOSTIC_PASS / Spec PARTIAL**（2026-09-21 23:59 -05:00）：
Core外置顺序/授权/失败取消修复后，8场景163 assertions三次通过；真实两节点Qwen
1024预算运行生成10token并EOS结束，输出`Hello! How can I assist you today?`，
两Provider完成、checkpoint prefix23、C++缓存诊断与cleanup均PASS，无残留。
完整Repo资格与sanitizer仍未完成，未混合提交或追加实验。见
[r257](evidence/b189-r257-core-assignment-externalization.md)。

**r255/r256 scope checkpoint — PARTIAL**：缓存诊断独立verdict已实现；C++ CLI24例、
Python93项通过，旧r254 raw以新oracle只读重判为CACHE_DIAGNOSTIC_PASS，完整Repo
资格仍NOT_RUN，原始失败记录未覆盖。见 [r255](evidence/b189-r255-cache-diagnostic.md)。
用户追加连续多token生成并更正为EOS/EOT或token上限，不需字符上限；字符草稿已
撤回，未为字符草稿构建/运行。1024预算贯通已完成静态复审、C++602 assertions三次、
预算18及builder177 assertions、CLI41 cases、Python94项通过；新1024实验在两ACK后
Core plan commit失败，未进入组装/生成，cleanup PASS无残留，保持PARTIAL；见
[r256](evidence/b189-r256-multitoken-stop.md)。

**r254 finalize/feedback — PARTIAL**（2026-09-21 22:40 -05:00）：按用户限定只修
正常 finalize 被标 STOP、反馈边缺 accepted planDigest 两项。生产补丁已写，
冻结复审、26.255s定向构建通过；C++4/4、345 assertions三次通过，安装一致。
随后唯一两节点运行返回token14582/Question、checkpoint；两Provider完成均observed，
反馈摘要正确、尾terminal齐备。oracle新首边界MATERIAL_FETCH/incomplete-material-sequence，
cleanup PASS、无残留；未追加修复或重跑。完整资格仍未通过。
见 [r254](evidence/b189-r254-finalize-feedback.md)，不新增无关重构或关闭验收任务。

**r253 finalization observation — PARTIAL**：已实现每轮日志偏移/共享截止时间及
Provider 真实收尾观测屏障；不提前终态、不替代原生 oracle。v2 静态复审通过，
85项编排测试通过（0.65s）；见 [r253](evidence/b189-r253-provider-finalization.md)。

**r252 evidence lines — PARTIAL**：已统一主要 Provider 机器日志 sink，增加真实
C++ 并发大记录/换行测试及 worker 链接闭包；首轮静态复审发现旧 stdout-only
observer 与异步缺 worker 提示遗漏，已修复，v2 五 lane STATIC_PASS；定向增量
构建成功3m2.476s；C++ 2/2、228 assertions 连续三次通过，CLI observer5/5通过。
另确认 requester exit 不保证 Provider terminal 已可见，需独立收尾屏障。
见 [r252](evidence/b189-r252-evidence-lines.md)，不改变产品任务验收状态。

**r251 wrapper exit — PARTIAL**：r250 两 Provider 已 execution completed，requester
返回 token14582（Question）、1 stream event 和 conversation checkpoint；launcher
因成功 marker 先于 poll 导致 returncode=None 被误判失败。cleanup PASS、无残留。
已补 poll/reap→读log 顺序和 wrapper 正反例，静态复审及33项测试通过；
相同 native identity 的 r251 再次原生成功，但 C++ oracle 拒绝交错拼接的 grant 日志；
cleanup PASS、无残留。下一步修日志原子性并核对 checkpoint 完成观测；
见 [r251](evidence/b189-r251-requester-exit.md)。未计完整资格验收。

**r250 epoch dataflow — PARTIAL**：已补逐轮精确 endpoint 枚举/选择、epoch0
readiness、最终 activation 在状态 commit 后传播，并修上游成功返回前的 KV retain。
冻结静态复审与限定构建通过；finalization 4/4、182 assertions 连续三次通过，含
四失败分支与两端 promotion/readback；epoch projection 23/23、128 assertions 通过。
builder roundtrip 首次因旧 fixture 缺必填字段失败，修测试副本并独立复审/增量构建后
1/1、163 assertions 通过。安装核对及实际原生请求成功，wrapper 收尾误判见上；
见 [r250](evidence/b189-r250-epoch-dataflow.md)。

**r249 half logits**：原生 lastLogits 已加入 FLOAT16 数值转换，保留 FP32 和 wire；
新增真实 coordinator C++ fixture、typed logits 正反例及独立 target。
五条 coverage lane 静态复审通过，限定增量编译通过（32.896s）；新增 C++ selector
3/3、125 assertions 通过；见 [half logits](evidence/b189-r249-half-logits.md)。
安装哈希一致；两节点诊断退出 1，Provider-1 报 GENERATION_EPOCH_LINEAGE_MISMATCH，
cleanup PASS、无残留；下一步静态核对 lineage 字段，任务保持 PARTIAL。
事后静态复审已确认 PIPELINE finalization 过滤遗漏、marker 不携带下游计算数据，
以及 V3 pipeline projection 未使用 epoch 参数；下一批统一修正对象身份与最终状态
推进并增加双角色 C++ 回归。未放宽校验、未再次运行；具体 mismatch 字段尚未取证。

**r248 evidence wiring — 2026-09-21 21:26 -05:00**：r247 因缺少必要 terminal
日志保留现场后有序停止（session 46086，退出 254），不是新业务成功或超限。
已补 launcher 默认 WARN 和配置/失败早停测试，静态复审待返回；
详见 [terminal evidence](evidence/b189-r248-terminal-evidence.md)。
下一步有界诊断获取真实首个失败原因，产品任务保持 PARTIAL。
后续实际结果：27 项 launcher 测试通过；r248 自动捕获 Stage/1
`native epoch coordinator logits are invalid` 后退出 1，cleanup PASS。
确认模型 logits 为 FLOAT16，而 C++ sampler 只接受 Float32；下一步兼容
采样数值转换并补 C++ 正反例。当前没有活跃 MiniNDN run，不关闭产品任务。

**r246 feedback checkpoint**：两个 Provider 缓存命中并完成 runner 创建，
position policy 修复在真实调用中生效；仍未返回 token。
线程栈捕获下游 TOKEN_FEEDBACK 被作为本机 producer 等待；已保留现场并
有序停止 run worker，session 53292 退出 254，不是资源超限。
下一步最小修复 control-edge 的缺映射 fallback 并做 C++ 回归；
详见 [r246 feedback evidence](evidence/b189-r246-feedback-route.md)，产品任务 PARTIAL。
后续 r247：最小 caller 修复及 partial/complete assignment C++ 用例已写入，
不可变快照只读单元/组合复审通过；affected build 通过（1m39.402s），
C++ 6/6 cases、74/74 assertions 通过。DI/provider 已定向安装且摘要一致；
实际跨节点反馈复验待启动，不能记为反馈链路 PASS。
复跑门通过后已启动 `two-provider-feedback-route-r247`，exec session `46086`，
日志 `.codex-tmp/spec189-r247-feedback-route/launch.log`；source cache 校验通过，
模型返回尚未确认。后续轮询同一 session，不重复启动。
最近观察：两个 runner 创建完成，无 token；provider-1 栈未见活动执行，
且 launcher 默认未配置 NDN_LOG，必须的 stage/terminal 日志缺失。
下一步核实请求终态与日志配置，不能沿用 r246 的 self-wait 结论。

**r245 terminal checkpoint**：exec `7992` 退出 1，`RESOURCE_BOUNDARY:ownedSwap`，
cleanup PASS。provider-0 缓存命中并完成 runner 创建；provider-1 冷组装完成，
最后边界 FACTORY_BIND_BEGIN，尚无 token。下文 running 描述为此前观察，
以 [terminal evidence](evidence/b189-r245-resource-boundary.md) 为准。
下一步定位绑定校验/异常传播及资源峰值；不放宽门限，不关闭任务。
静态对照已确认 position policy 没有按角色实际输入投影：合法 stage-1
没有 mask/position 输入，但 C++ binding 强制要求；历史 Python 按本地输入
决定是否启用。2026-09-21 21:04 -05:00：C++ role-local 修复与正反例已编写，
独立只读单元/组合复审通过；affected DI 库与 `spec189-preparation-alias`
构建通过，C++ 5/5 cases、61/61 assertions 通过。Provider 独立编译闭包补审后
也已定向构建/安装，产物与 installed 摘要一致。真实模型复验待运行；
swap 和失败传播仍开放，详见上述 terminal evidence。
同一 reviewer 完成 rerun gate 后已启动 `two-provider-position-policy-r246`
（exec session `53292`，日志 `.codex-tmp/spec189-r246-position-policy/launch.log`），
保持原门限与缓存校验；运行结论待观察，任务保持 PARTIAL。

**Installed Python closure repair**：已删除 launcher 对 DI checkout 的主动路径注入，
通过原有 wheel 入口安装 root 所需 DI/NDN/MiniNDN 模块。只读静态审查通过；
root imports、native wrapper 安装和 loader 全局闭包已核对。
详见 [installed closure](evidence/b189-installed-python-closure.md)。下一步观察
已固定两节点参数的新 raw run，产品任务保持 PARTIAL。

本轮 launcher/cache tests 30/30 PASS，root 模块来源核验已留档。
后续更新：native wheel 构建、安装与 loader 核验已通过；诊断 run
`two-provider-installed-python-r245` 正在 exec session `7992` 执行，结果待观察。
已观察两个 Provider 的真实 placement ACK；两段均到达 `ASSEMBLY_CALL_DONE`，
provider-1 最后观察边界为 `FACTORY_BIND_BEGIN`，尚无 token 返回结论。

**Review workflow cleanup**：本轮扫描仓库 Markdown/RST/TeX 及 active Spec、
`.specify` 文档配置，未发现 DeepSeek 复核或 delegate 流程残留；不使用该流程。
保留模型兼容性说明和独立 Claude 后端配置，不改动会话数据。
该检查不构成模型验收，也不关闭产品任务。

**B189 r244 v5 caller preflight**：两段 manifest 被手工启动命令传成三个节点，
退出 1、cleanup PASS、无 MiniNDN 执行。已确认下一调用须使用 `ucla,arizona`。
host guard 定向测试 22/22 通过；worker 定向安装成功。
此前关于 tokenizer 缺失与 worker 未安装的诊断不成立，详见
[v5 evidence](evidence/b189-r244-v5-launch-preflight.md)。
产品任务仍为 PARTIAL；下一步固化可复用调用并核对安装闭包后重试。

v5 后续静态修复：默认/示例改为两执行节点；LocalExperiment 补齐六个程序路径
传递及 oracleBinary 的安装路径配置，解除“只传摘要却使用构建树默认程序”的
caller 错误。只读复审通过，配置/缓存/host guard 52 项通过，日志见同一
[v5 evidence](evidence/b189-r244-v5-launch-preflight.md)。Python checkout 导入与
root 缺 ndn 的安装闭包仍开放，未重跑模型，任务 checkbox 不变。

**B189 r244 cross-process cold-assembly gate — 2026-09-21**:
为两个 Provider 的 model-sized cold assembly 增加共享 advisory `flock` admission
gate。cache hit 不等待；cache miss 在 `prepareNativeCanonicalOnnxRole` 入口取得
共享锁，等待期间检查取消、request deadline 和 assembly timeout，并记录
`COLD_ASSEMBLY_WAIT`/`COLD_ASSEMBLY_ENTERED`；RAII 覆盖异常和 cleanup。直接 assembler
调用的默认锁路径从 cache root 父目录派生，修复普通用户访问历史 root-owned
`/var/tmp` 路径的权限边界；生产 Provider 仍按固定 artifact-cache root 共享一把锁。
affected build/install 通过，preparation `4/4`、canonical publisher `16/16`、
provider assembly `20/20` 通过，注册双 Provider assembly selector 至少观察到
`COLD_ASSEMBLY_GATE state=ENTERED`。真实 MiniNDN 新 run 只到两 Provider `PROVIDER_READY`，
外层会话随后以 `137` 结束且无 guard/assembly 终点；没有把它记为 runtime PASS。
证据见 [r244 cold-assembly gate evidence](evidence/b189-r244-cold-assembly-gate-20260921.md)。
T003、T005、T006、T007、T009 仍保持 `PARTIAL`，下一步先修复/诊断 launcher
session `137` 边界，再重跑真实两 Provider 链路；不改变当前内存门限。

**B189 r242 Python/C++ model-path parity repair — 2026-09-21**:
完成旧 Python Qwen 成功路径与当前 C++ production path 的静态逐阶段对照。
已确认 `input_ids` request tensor bundle、C++ 生成的 `attention_mask`/
`position_ids`、stage concrete boundary/passthrough、`past/present` KV、命名
`logits` 输出和 7000-byte publication contract 对齐；recipe-addressed assembled
cache 保留。唯一确认的运行时不一致是 decode epoch 重取首轮
`APPLICATION_INPUT`，现已修复为 epoch 0 仅消费 ingress、epoch > 0 仅消费
`TOKEN_FEEDBACK` 并使用 Provider-local KV。affected C++ build/install 通过；
preparation 4/4、canonical publisher 16/16、provider assembly 20/20 通过。
证据见 [r242 parity evidence](evidence/b189-r242-python-cpp-static-parity-20260921.md)。
新增 coordinator regression 尚未由 aggregate `unit-tests` 编译，因无关的旧
`di-native-planning.t.cpp` 聚合赋值错误阻塞；没有修改该 stale test。T003、T005、
T006、T007、T009 仍为 `PARTIAL`，下一步用新 raw run 验证真实两 Provider handoff
和 terminal。

**B189 r232 preparation observability and alias-contract boundary — 2026-09-21**:
Provider executable 已补充 `CACHE_LOOKUP`、`ASSEMBLY_CALL`、factory bind 进度，
assembler 已补充 source/initializer/worker/cache-finalization 进度及异常首行。新的
installed-candidate raw run 进入真实 MiniNDN；Provider-0 完成 worker assembly 和
`model.onnx` 写入，但在 `bindNativeRunnerOutputScopes` 因
`native runner activation output alias contract is ambiguous` 失败。没有
`RUNNER_SPEC_READY`、`RUNNER_READY`、ORT、terminal 或 two-provider qualification
证据，operator 停止后 cleanup 无残余进程，保持 `RUNTIME_UNQUALIFIED`，不推进任务
checkbox。详细边界见 [r232 evidence](evidence/b189-r232-preparation-alias-contract-20260921.md)。
下一步只核对并修复 authenticated generation state contract 从 Core sealing 到
Selection projection/Provider assembler 的传递，保留 alias 校验，再做 affected
C++ build、selector 和新的 raw run。

r233 首次启动另有一个 launcher preflight-only 失败：手工传入 binary digest 时遗漏
`sha256:` 前缀，返回 `CONTROLLER_BINARY_DIGEST_MISMATCH`；未启动 MiniNDN、Core、
Provider 或模型路径，不能计入 runtime 结果。实际 Controller hash 与候选一致，命令
格式已修正；产品任务仍不推进。

r233 retry 已通过 digest preflight 并进入 MiniNDN；诊断确认 generation contract
完整到达 Provider，但 alias 两侧计数为 `activationOutputs=4`、`semanticOutputs=1`，
原因是 activation 侧未排除 3 个 passthrough/local-input tensor。运行资源正常、guard
未触发；operator 停止后没有 runner/ORT/terminal 证据，仍保持
`RUNTIME_UNQUALIFIED`。下一步修正 assembler 与 cache/rebind preparation 的共同
local-input 过滤规则，补回归 case 后再 build、selector、raw run。

r234 修复的第一次 affected build 在 `NativeRunnerPreparation.cpp` 因 rebind 函数
缺少本地 `localInputNames` 集合而失败；未安装、未运行 selector 或 MiniNDN，任务仍
未完成。失败边界已记录，下一步仅补齐局部集合并重编。

r234 第二次 affected build/install 通过；新增 `spec189-preparation-alias` 为 `4/4`
（44 assertions），`Spec185ProviderAssembly` 为 `20/20`（192 assertions），
`spec189-canonical-publisher` 为 `16/16`（397 assertions）。这是 focused/native
selector PASS，不推进 Qwen two-provider checkbox；下一步运行新的 installed raw
candidate 验证 `RUNNER_READY`、ORT、terminal 和 cleanup。

r235 使用该 installed candidate 的新 raw run 仍在 Provider-0 alias contract 边界停止：
`generationEnabled=true generationStateOutputs=56 expectedOutputs=32 localInputs=31
roleOutputs=1 activationOutputs=4 semanticOutputs=1`。因此实际多出的 3 个 concrete
output 名称不等于当前 projection 的 local-input 名称，r234 过滤修复尚未解除生产
边界；资源 guard 未触发，未取得 runner/ORT/terminal 证据。下一步增加名称级诊断，
核对 authenticated semantic edge 到 ONNX concrete name 的映射后再做最小 C++ 修复。

**B189 recipe-addressed assembled-cache checkpoint — 2026-09-21**:
静态对照确认 Python launcher 负责 canonical source/initializer、node mapping、
layer/state contract 和 Provider command；C++ `NativeCanonicalRolePreparer` 与
`NativeCanonicalOnnxAssembler` 负责 Selection 后的真实 role assembly、pinned
worker 和 ORT runner。assembled cache 已修正为组装前可知的
`recipeDigest` 目录键：`assembled/<safe-role>/<recipeDigest>/model.onnx`；
完成后的 model SHA 只通过 `manifest.json` 和文件 SHA-256 做命中校验，不再扫描
同 role 下未知 SHA 目录。backend/ABI 等仍保留在 recipe，因为当前缓存交付的是
可运行 contract；普通 protected Repo 仍是 grant-bound encrypted path。受影响
build `BUILD_RC=0`，`Spec185ProviderAssembly` `20/20 PASS`，Python launcher/cache
定向测试 `29 passed`，`git diff --check=PASS`。这只是 `FOCUSED_BEHAVIOR_PASS`，
不推进任何 product checkbox；r229 之后仍为 `RUNTIME_UNQUALIFIED`。证据见
[recipe-key cache checkpoint](evidence/b189-cache-recipe-key-20260921.md)。下一步
只运行新的 installed-candidate MiniNDN raw run，验证真实 cache hit、runner、ORT、
terminal 和 cleanup 边界。

同一 checkpoint 已补上模型准备的单元/selector 证据：固定种子生成并重新读取
小型 4-node fully-connected ONNX，生产 `NativeCanonicalRolePreparer` 将其分为
3+1 两个 role，并由 native assembly 链分别完成 full checker、shape inference、
CPU ORT session load；测试结束后 `/tmp/spec189-generated-multilayer-fc` 不存在。
`spec189-canonical-publisher` 全部 `16/16`、`397 assertions` 通过，模型准备
定向 case `1/1` 通过。该证据只证明准备/拆分/组装模块边界，不推进真实 Qwen
two-provider product checkbox；本轮显式路径 checkpoint 尝试仍被仓库 hook 以
`development-assistant files or references remain in the Git index` 拒绝，未绕过、未
push；下一步仍只运行新的 installed-candidate MiniNDN raw run。

**B189 r229 cache-compatibility assembled-cache eligibility boundary — 2026-09-21**:
r229 使用修正后的 Qwen manifest 和 `hashesVerified=true` 的 system-wide
model-source cache，在显式 cache-compatibility mode 下进入真实 MiniNDN。两 Provider
完成 signed placement ACK 和 grant verification；Provider-0 到达
`PROTECTED_RUNTIME_FACTORY_DONE`、`ROLE_SPEC_READY`、`EPOCH_COORDINATOR_BEGIN`、
`RUNNER_PREPARATION_BEGIN`、`RUNNER_PREPARATION_FACTORY_BEGIN`，Provider-1 到达
`EPOCH_COORDINATOR_BEGIN`。运行在首个稳定边界后由 operator 以 `143` 停止；这不是协议
结果。389 个 resource samples 中 `ownedSwapBytes` 最大 `40439808`、最低可用内存
`3570765824` bytes、最低 free disk `37422333952` bytes、最大 supervised RSS
`5490577408` bytes，host guard 未触发且 cleanup 后无残留进程。没有 assembly worker、
`RUNNER_READY`、ORT、terminal 或 two-provider qualification 证据，保持
`RUNTIME_UNQUALIFIED`，不推进 checkbox。证据见
[r229 runtime evidence](evidence/b189-r229-cache-compatibility-factory-diagnostic-20260921.md)。

静态检查发现原 hash-only assembled lookup 只接受 `plaintext-v1`，因此本次仍带
authenticated protected epoch 的 cache-compatibility run 没有命中已组装模型，退回了
大模型 cold assembly。修复现已限制为：显式 cache-compatibility mode 下，Selection/grant
通过后仅按 `assembled/<safe-role>/<sha256>/model.onnx` 的实际 SHA-256 命中；protected
cold assembly 只在该诊断模式写入可复用明文，命中时跳过第二份 protected staging；普通
protected Repo 仍保持 grant-bound encrypted 语义。受影响 Waf build 与
`Spec185ProviderAssembly` 19/19 通过。下一步先冻结新的 post-repair static snapshot，
再用新的 raw run 验证 cache hit；正式 Qwen two-provider qualification 仍需原始
protected Repo 链路，不能由诊断模式计为完成。

**B189 r228 in-place shape and streamed-response Core timeout — 2026-09-21**:
r228 使用修正后的 maintained Qwen stage manifest，通过 hash-only model-source
cache 校验并进入真实 MiniNDN。两 Provider 都完成 grant verification 并到达
`EPOCH_COORDINATOR_BEGIN`；Provider-0 继续到
`RUNNER_PREPARATION_BEGIN` 和 `RUNNER_PREPARATION_FACTORY_BEGIN`。随后 requester
在 Core 边界报告 `NATIVE_REQUEST_TIMEOUT`，Core request deadline expired；host
resource guard 未触发，`ownedSwapBytes=124108800`、最低可用内存为
`4033961984` bytes，最大采样进程 RSS 为 `2728910848` bytes。没有 assembly
worker、`RUNNER_READY`、ORT、terminal 或 two-provider qualification 证据，cleanup
完成且无残留进程，保持 `RUNTIME_UNQUALIFIED`，不推进 checkbox。in-place shape
inference 与 streamed worker response 的 native build 通过，`Spec185ProviderAssembly`
19/19；hash-only cache 规则不变。证据见
[r228 runtime evidence](evidence/b189-r228-inplace-shape-core-timeout-boundary-20260921.md)。
下一步只检查 Core/provider preparation wait 及 deadline/lease progression，不改变
cache 语义、不把 timeout 计为模型推理结果。

**B189 r227 stage-manifest preflight boundary — 2026-09-21**:
r227 在 MiniNDN 启动前因输入 manifest 缺少显式 `modelFamily` 失败，报告
`ValueError: stage manifest requires an explicit modelFamily`；没有 Provider、Repo、
ACK、Selection、assembly、ORT 或资源结果。r228 已使用包含
`"modelFamily": "qwen"` 的 maintained manifest 修正该输入边界，产品任务仍保持
`RUNTIME_UNQUALIFIED`，不推进 checkbox。证据见
[r227 preflight evidence](evidence/b189-r227-manifest-preflight-boundary-20260921.md)。

**B189 r226 finalization-scope repair and worker-memory boundary — 2026-09-21**:
r226 通过完整 preflight 和 hash-only model-source cache 校验；两 Provider 产生 signed
placement ACK。finalization mutex 修复解除 r225 自死锁，Provider-0 进入
material-backed role preparation 并生成 `752094842` bytes 的 staged
`canonical.onnx`；Provider-0 到达 `RUNNER_PREPARATION_FACTORY_BEGIN`，Provider-1
到达 `EPOCH_COORDINATOR_BEGIN`。随后 host guard 因
`ownedSwap=482988032` 超过 `268435456` 停止，最低可用内存为
`2230837248` bytes，最大 supervised child RSS 为 `3621294080` bytes，cleanup=PASS
且无残留进程。没有 completed assembly worker、`RUNNER_READY`、ORT、terminal 或
two-provider qualification 证据，保持 `RUNTIME_UNQUALIFIED`，不推进 checkbox。证据见
[r226 runtime evidence](evidence/b189-r226-finalization-scope-worker-memory-boundary-20260921.md)。
下一步只降低 native assembly-worker/ORT 首次工作集，并保持 authenticated fetch、C++
production worker、resource guard 和 hash-only cache 规则不变。

**B189 r225 material-payload release and finalization deadlock — 2026-09-21**:
r225 通过完整 preflight，复用并校验同一 hash-only model-source cache；两 Provider
产生 signed placement ACK，Provider-0 到达 `RUNNER_PREPARATION_FACTORY_BEGIN`，
Provider-1 到达 `EPOCH_COORDINATOR_BEGIN`。按 authenticated material reference
最后一次消费释放 payload backing 后，resource samples 的
`ownedSwap` 峰值为 `88768512`，最低可用内存为 `3450798080` bytes，未触发
resource boundary。只读 gdb 随后确认 Provider-0 在
`NativeAssemblyArtifactDirectoryOwner` 析构中重新获取仍由 preparation path 持有的
`nativeAssemblyFinalizationMutex`，形成自死锁；运行由 operator 有界停止，cleanup
完成且无残留进程。没有 `RUNNER_READY`、ORT、terminal 或 two-provider qualification
证据，保持 `RUNTIME_UNQUALIFIED`，不推进 checkbox。证据见
[r225 runtime evidence](evidence/b189-r225-material-payload-release-finalization-deadlock-20260921.md)。
下一步仅将 finalization mutex 缩到 immutable directory file-finalization 区间，再用
新的 raw run 验证；hash-only cache 规则不变。

**B189 r224 serialization-memory repair boundary — 2026-09-21**: r224 通过完整
launcher preflight，`MODEL_SOURCE_CACHE` 报告 `hashesVerified=true`，并进入真实
MiniNDN；两 Provider 都产生 signed placement ACK 和
`GRANT_VERIFICATION status=VERIFIED boundary=BEFORE_ASSEMBLY`。Provider-0 到达
`RUNNER_PREPARATION_FACTORY_BEGIN`，Provider-1 到达 `EPOCH_COORDINATOR_BEGIN`，但
没有 `RUNNER_READY`、ORT、terminal 或 two-provider qualification 证据。新增的直接
protobuf vector serialization 已完成受影响 native build、安装身份核对和
`Spec185ProviderAssembly` 19/19 focused 测试；运行时仍由 host guard 因
`ownedSwap=303005696` 超过 `268435456` 上限停止。最低可用内存为
`2243293184` bytes，cleanup=PASS 且无残留进程，因此这是 swap working-set 边界，
不是 available-memory floor。保持 `RUNTIME_UNQUALIFIED`，不推进 checkbox。证据见
[r224 runtime evidence](evidence/b189-r224-serialization-memory-repair-boundary-20260921.md)。
下一步只处理首次 protected material 装配的 working-set/lifecycle，并保留完整
authenticated publication/fetch、assembly、runner 和 terminal 路径；hash-only cache
不能作为 protected epoch 的 qualification 替代。

**B189 r223 epoch/material fetch boundary — 2026-09-21**: r223 通过完整
launcher preflight 并进入真实 MiniNDN；hash-only model-source cache 校验成功。两
Provider 都到达 `EPOCH_COORDINATOR_BEGIN`，Provider-0 继续到
`RUNNER_PREPARATION_FACTORY_BEGIN`。只读 gdb 栈确认当前生产等待位于
`ServiceProvider::fetchAndDecryptLargeDataUntil` →
`CollaborationContext::fetchEncryptedLargeData`，由 native canonical assembler
请求 `material-bundle`。host guard 随后因 `ownedSwap=384675840` 超过
`268435456` 上限早停；cleanup=PASS、无残留进程。Provider-0 staging 中的
`canonical.onnx` 仅是失败现场证据，不是 cache hit、runner、ORT、terminal 或
two-provider PASS。保持 `RUNTIME_UNQUALIFIED`，不推进 checkbox。证据见
[r223 runtime evidence](evidence/b189-r223-epoch-material-fetch-boundary-20260921.md)。
下一步只修正 authenticated material publication/fetch 与首次装配的内存/cache
生命周期；qualification 不得改走 cache-compatibility mode。

**B189 r221 status/epoch boundary — 2026-09-21**: r221 通过 preflight、model-source cache
校验并进入真实 MiniNDN；两 Provider 都完成 grant/binding、`ASSEMBLY_ADMISSION_DONE`、
`ROLE_SPEC_READY` 和首个 `STATUS_REPORT_DONE`，但没有第二个状态、`EXECUTION_ENTERED`、
`ASSEMBLY_STARTED`、`RUNNER_READY`、ORT、execution 或 terminal 证据，也没有启动 assembly
worker。操作员有界停止后 supervisor 为 `cleanup=PASS` 且无残留进程。该取消不是协议结果，
保持 `RUNTIME_UNQUALIFIED`。证据见 [r221 runtime evidence](evidence/b189-r221-status-boundary-20260921.md)。
下一步只在 authenticated epoch coordinator 入口和 `runnerPreparationFactory` 调用前后加入
直接边界探针，再用新的 raw run 定位首个未观测区间。

**B189 r219 post-grant boundary — 2026-09-21**: r219 通过完整 launcher preflight 并进入真实
MiniNDN；hash-only model-source cache 报告 `hashesVerified=true`，两 Provider 都打印了
protected-runtime factory、grant verification、binding validation、`GRANT_STAGE_MARKER_DONE`
和 `POST_GRANT_CONTINUING`。之后没有 assembly admission completion、`ASSEMBLY_STARTED`、
`RUNNER_READY`、ORT、execution 或 terminal 证据，也没有启动 assembly worker；有界停止后
supervisor 为 `cleanup=PASS` 且无残留进程。该取消是诊断停止，不是协议结果，保持
`RUNTIME_UNQUALIFIED`。证据见 [r219 runtime evidence](evidence/b189-r219-post-grant-boundary-20260921.md)。
下一步只在 assembly admission reporting、lease activation 与首个 readiness/execution status
调用前后增加直接边界探针，再用新 raw run 定位首个未观测区间。

**B189 r217 post-grant probe runtime stop — 2026-09-21**: r217 通过完整 preflight 并
进入真实 MiniNDN；两 Provider 都有 signed ACK 和
`NDNSF_DI_GRANT_VERIFICATION status=VERIFIED boundary=BEFORE_ASSEMBLY`，但没有
assembly worker、`ASSEMBLY_STARTED`、`RUNNER_READY`、ORT、execution 或 terminal 证据。
为避免重复等待未改变的长 Core deadline，诊断运行约五分钟后停止；supervisor
`cleanup=PASS` 且无残留进程。该取消是操作员诊断停止，不是协议结果，保持
`RUNTIME_UNQUALIFIED`。证据见 [r217 runtime evidence](evidence/b189-r217-post-grant-probe-runtime-stop-20260921.md)。
下一步在 protected-runtime factory 返回、binding validation 和 post-grant marker
前后加入直接 flush 的 Provider stdout 探针，再用新的 raw run ID 定位首个边界。

**B189 r216 stage-manifest stop-contract preflight boundary — 2026-09-21**: r216 通过了
摘要和 `modelFamily` 检查，但 `candidate/stage-manifest-qwen-r99.json` 缺少 launcher
要求的显式 `eosTokenIds`，首先报告 `ValueError: MODEL_EOS_TOKEN_IDS_REQUIRED` 并在
MiniNDN 启动前停止。无 Controller、Provider、ACK、Selection、Repo、assembly、ORT、
terminal 或模型证据；这是 `PREFLIGHT_UNQUALIFIED`，不推进 checkbox。证据见 [r216
preflight evidence](evidence/b189-r216-stage-manifest-stop-contract-preflight-boundary-20260921.md)，
下一次使用已核对同时包含 `modelFamily` 与 `eosTokenIds` 的
`candidate/stage-manifest-qwen-v2.json` 和新的 raw run ID 继续。

**B189 r215 stage-manifest schema preflight boundary — 2026-09-21**: r215 通过了
带 `sha256:<hex>` 的文件摘要检查，但选择的 `candidate/stage-manifest.json` 缺少
launcher 要求的显式 `modelFamily`，在加载 manifest 时首先报告
`ValueError: stage manifest requires an explicit modelFamily`。无 MiniNDN、Controller、
Provider、ACK、Selection、Repo、assembly、ORT、terminal 或模型证据；这是
`PREFLIGHT_UNQUALIFIED`，不推进 checkbox。证据见 [r215 preflight evidence](evidence/b189-r215-stage-manifest-schema-preflight-boundary-20260921.md)，
下一次使用已核对的 Qwen manifest `candidate/stage-manifest-qwen-r99.json` 和新的 raw
run ID 继续。

**B189 r214 stage-manifest digest preflight boundary — 2026-09-21**: r214 在
MiniNDN 启动前因命令把 `sha256sum` 的裸十六进制摘要传给要求
`sha256:<hex>` 的 launcher，首先报告 `MODEL_STAGE_MANIFEST_DIGEST_MISMATCH`。
无 Controller、Provider、ACK、Selection、Repo、assembly、ORT、terminal 或模型证据；
这是 `PREFLIGHT_UNQUALIFIED`，不推进任何 checkbox。新的 post-grant C++ 探针已在此
之前安装，focused `Spec185ProviderAssembly` 为 19/19；该结果不能改变 r211 的
`RUNTIME_UNQUALIFIED`。证据见 [r214 preflight evidence](evidence/b189-r214-stage-manifest-preflight-boundary-20260921.md)，
下一次使用新的 raw run ID 和带 `sha256:` 前缀的完整摘要继续同一链路。

**B189 hash-only cache static recheck — 2026-09-21 11:31 -05:00**: 重新核对
`NativeCanonicalOnnxAssembler`、`Provider` 调用方、C++ fixture 和 build closure。
当前 plaintext cache 只在 authenticated Selection/grant 后按
`assembled/<role>/<sha256>/model.onnx` 的目录名与实际文件 SHA-256 命中；不再读取
`manifest.json`/`manifest.signature`。确认 `artifactDigest` 不是 assembled 文件摘要，
因此没有把它错误用作缓存键；同时修复 `sha256File` 在小上限/并发文件增长边界上的
无符号下溢检查。r213 又覆盖了通用 `readFile()` 的同类边界；该单元现在是
`FOCUSED_BEHAVIOR_PASS` 的 cache 复核，仍未推进
任何 Spec189 qualification checkbox。验证与限制见 [assembled-model cache evidence]
(evidence/b189-assembled-model-cache-20260921.md)；下一步继续定位 r211 的
`ACK/grant → Selection/assembly/runner → Core timeout` 首个生产边界。

本轮本地 checkpoint commit 被仓库 hook 拒绝，原因是当前 index 中仍有
development-assistant 文件/引用；未绕过 hook，现有显式暂存保持不变。该提交门禁
不改变本单元的 focused validation 结果，也不推进任何 qualification checkbox。

**B189 r211 BASIC assembly-worker Core timeout boundary — 2026-09-21**: 新 closure
运行 r211 通过 model-source hash cache，两个 Provider 完成 signed ACK 和
`GRANT_VERIFICATION boundary=BEFORE_ASSEMBLY`；BASIC worker 修复使
`ownedSwapBytes` 峰值保持在 `212910080`，未重现 r209 的资源早停。但 Requester
随后报告 `NATIVE_REQUEST_TIMEOUT boundary=Core`，没有 `RUNNER_READY`、ORT、terminal
或 two-provider PASS，supervisor cleanup PASS。证据见 [r211 evidence](evidence/b189-r211-basic-worker-core-timeout-boundary-20260921.md)。
保持 `RUNTIME_UNQUALIFIED`；下一步只追查 post-grant 到 Selection/assembly/runner/
response 的 Core 推进缺口，不调整资源阈值、不把 timeout 重跑计为完成。

**B189 assembly-worker peak-memory repair — 2026-09-21**: 针对 r209 的
`RESOURCE_BOUNDARY:ownedSwap`，assembly worker 保留 full ONNX checker 和真实 CPU
ORT session load，但把 ORT graph optimization 从 `ORT_ENABLE_ALL` 对齐为正式
`OnnxRuntimeModelRunner` 已使用的 `ORT_ENABLE_BASIC`，以去除验证阶段不必要的
transient graph/weight peak。受影响 DI native targets build 通过，
`Spec185ProviderAssembly` 19/19 通过；新 closure 已安装到 `/usr/local`，receipt
`SPEC180_NATIVE_IDENTITY_OK`、Python imports、RUNPATH/`ldd` closure 通过。该修复尚未
经过新的 MiniNDN runtime run，故仍不推进产品 checkbox；下一步使用新的 raw run ID
验证资源峰值及完整链路。

**B189 r209 owned-swap assembly boundary — 2026-09-21**: r209 通过
`MODEL_SOURCE_CACHE hashesVerified=true`，两个真实 Provider 完成 signed ACK 和
`GRANT_VERIFICATION boundary=BEFORE_ASSEMBLY`；随后 native assembly worker 出现
约 4.12 GiB RSS，launcher 首次超过 `maxOwnedSwapBytes=256 MiB`，报告
`RESOURCE_BOUNDARY:ownedSwap` 并完成 cleanup。未产生 `RUNNER_READY`、ORT、terminal
或 two-provider PASS；Requester 的 `CANCELLED` 是资源门禁后的结果。raw 资源样本、
Provider/Requester 日志和边界分析见 [r209 evidence](evidence/b189-r209-owned-swap-assembly-boundary-20260921.md)。
保持 `RUNTIME_UNQUALIFIED`；下一步只修正 assembly worker 峰值内存并做定向 C++
验证，然后使用新的 raw run ID 重跑。

**B189 r208 launcher variable-order preflight boundary — 2026-09-21**: r208 在
`MODEL_SOURCE_CACHE hashesVerified=true` 后、MiniNDN 启动前因新预算代码使用尚未赋值的
`canonical_initializer_bytes` 抛出 `UnboundLocalError`，返回 `RC=1`。没有 Controller、
Requester、Provider、ACK、Selection 或模型证据；raw launch log 与 run root 已保留在
[r208 evidence](evidence/b189-r208-budget-variable-preflight-boundary-20260921.md)。
这是 launcher preflight bug，不推进任何 checkbox；修复变量赋值顺序后必须重新跑定向
Python 检查并使用新的 raw run ID。

**B189 hash-only cache admission repair — 2026-09-21**: 本地 plaintext assembled-model
cache 现在只在当前 authenticated Selection/grant 已通过后，按
`assembled/<role>/<sha256>/model.onnx` 的目录摘要与文件实际 SHA-256 判定命中；不再读取
或依赖 `manifest.json`、`manifest.signature`。当前请求的 runner/KV/lease 仍独立，protected
role 仍不进入 plaintext cache。受影响 native closure 已重建，`Spec185ProviderAssembly`
19/19 通过；fixture 明确覆盖“无 manifest 仍命中”和“篡改后删除精确 entry”。这只是
focused cache evidence，不推进 T003/T005/T006/T007/T009。

**B189 r207 preparation material-budget boundary — 2026-09-21**: r207 完成 root、安装
oracle 摘要、model-source hash cache 与两 Provider 启动；但在 ACK/Selection 前，Requester
于 preparation 首先失败：`SOURCE_IDENTITY_MISMATCH boundary=preparation`，底层
`DI_NATIVE_ONNX_MATERIAL_LIMIT`。本轮生成的 `max_assembled_bytes=1020532942` 仅覆盖
最大 stage 加余量，却低于 native canonical material bundle/manifest/receipt 共享的
`projection.assembly.maxAssembledBytes` 契约；因此不能把该值直接当作每个 stage 的最终
文件上限。supervisor cleanup PASS，Provider 只到 READY，未产生 ACK、Selection、assembly、
runner、ORT 或 terminal evidence。raw run 与证据见
[r207 preparation boundary](evidence/b189-r207-preparation-material-budget-boundary-20260921.md)。
保持 `RUNTIME_UNQUALIFIED`，下一步先修正并定向验证 native material budget，再生成新 raw run；
不得把 r207 计为模型或协议完成。

**B189 native-contract-aware budget repair — 2026-09-21**: launcher 现按
`max(4 * largest_verified_stage, canonical_source + canonical_initializer) + 256 MiB`
生成 `max_assembled_bytes`。其中前项覆盖 native role materialization 的 selected payload、
initializer copy 和双序列化 buffer，后项覆盖 preparation 阶段完整 material set；不是把
initializer 四次重复计入每个 Provider。当前 Qwen candidate 计算值为 `3276825400` bytes，
相应 `max_prepared_bytes` 约 5.98 GiB。`py_compile` 与 29 项 Python 定向测试通过；
尚未安装该新 launcher/native receipt，也未重跑 MiniNDN，因此不推进任何 checkbox。下一步
安装并核对新 native closure，再以新 raw run 验证真实链路。

**B189 r206 digest preflight boundary — 2026-09-21**: r206 在 MiniNDN 启动前因
手工转录的 `--oracle-binary-sha256` 错误而停止，报告
`SPEC189_ORACLE_BINARY_DIGEST_MISMATCH`；没有 Controller、ACK、Selection、Provider
或模型结果。raw root 已保留，实际已安装 oracle 摘要已重新读取并写入
[r206 evidence](evidence/b189-r206-budgeted-launch-preflight-boundary-20260921.md)。
这是 `PREFLIGHT_UNQUALIFIED`，不推进任何 checkbox；下一次使用新 r207 raw run 和
正确摘要验证刚修正的 per-role assembly budget。

**B189 per-role assembly budget correction — 2026-09-21**: 静态审查确认 launcher
曾按 `3*source + 4*initializer` 生成 `max_assembled_bytes`，把完整 external
initializer 错误计入每个 Provider 的单层输出上限和 preparation-cache reservation。
第一次修复改为“最大 stage + 256 MiB”，但 r207 证明该值又低于 native material
bundle/manifest/receipt 的共享 assembly budget，故该修复保持为失败尝试，不视为完成。
旧的 `max_source_bytes` 独立限制 canonical graph/initializer 的设计保留；当前待修正的是
`max_assembled_bytes` 的 native-contract-aware 上界。本轮不推进 T003/T005/T006/T007/T009。

**B189 r205 production-Repo/Core timeout boundary — 2026-09-21**: r205 使用已安装
的 hash-only cache closure、run-root 外的真实 Repo staging 和新的 raw run，完成
model-source cache 哈希校验并进入 MiniNDN。两个 execution Provider 都完成 ACK 和
`GRANT_VERIFICATION boundary=BEFORE_ASSEMBLY`；Provider-0 观察到约 752 MB 的
assembly staging 文件，但双方都没有 `ASSEMBLY_STARTED`、`RUNNER_READY`、ORT 或
terminal marker。Requester 首先报告 `NATIVE_REQUEST_TIMEOUT boundary=Core`，launcher
返回 `RC=1`；Repo staging 已清理，supervisor `cleanup=PASS`。证据见
[r205 boundary](evidence/b189-r205-hash-only-production-repo-boundary-20260921.md)。
保持 `RUNTIME_UNQUALIFIED`，不推进任何 checkbox；r207 的 budget boundary 已另行记录，
下一步使用 native-contract-aware budget 和新 raw run 重试。

**B189 local assembled-model cache simplified to hash-only admission — 2026-09-21**:
按用户要求，plaintext assembled cache hit 不再要求 `manifest.signature` 或
`manifest.json`；仍必须在当前 authenticated Selection/grant 路径内，校验
`assembled/<role>/<sha256>/model.onnx` 的实际 SHA-256。protected role 继续不进入该
plaintext reuse path。当前 affected native closure build 通过；`Spec185ProviderAssembly`
19-case C++ suite 在 `NDNSF_SPEC182_BIN_DIR=$PWD/build-spec189-oracle` 下通过，包含无
signature fixture 命中及篡改 model 后精确 entry 删除。只读复审无 P0/P1；本机 cache
不承担恶意本地进程防护，standalone provider 的 source tuple 与 hash/open TOCTOU
仍是已记录的 P2 边界。此 focused result 不推进 T003/T005/T006/T007/T009，也不改变
r203 `RUNTIME_UNQUALIFIED`；下一步继续核对当前 installed source closure，再重跑新的
MiniNDN production Repo 链路。

**B189 current installed-runtime receipt — 2026-09-21**: hash-only cache closure 已
安装到 `/usr/local`，Python bindings 使用安装库 digest 重装。新的
`.codex-tmp/spec189-current-native-build-20260921.json` 由仓库
`spec180_native_build.py build` 生成，并以相同 system-first PATH 通过
`spec180_native_build.py verify`、Python imports、Provider RUNPATH/`ldd` closure
检查。中间一次 `rg` PATH 错误和一次 `WAF_TOOL_CHANGED` 环境漂移已登记在
`docs/failure-log.md`，最终 verify 为 PASS。下一步才允许用该 receipt 启动新的
MiniNDN raw run；若首个生产边界失败，先保存 evidence 再重试。

**B189 r204 launcher path boundary — 2026-09-21**: r204 在模型 source cache 校验后、
MiniNDN 启动前停止，因为显式 `--encrypted-repository-path` 错误地位于 raw
`--run-root` 内，launcher 返回 `ENCRYPTED_REPOSITORY_PATH_MUST_BE_OUTSIDE_RUN_ROOT`。
无协议/模型结果，raw run 保留并已写入 failure log；下一次使用新 r205 raw run 及
run-root 外的 Repo staging，继续同一 production Repo 链路。

**B189 assembled-model cache verification — 2026-09-21**: 当前 Provider 已具备
system-wide content-addressed assembled-model reuse：在本次 authenticated
Selection/grant 后扫描 `assembled/<role>/<assembledModelDigest>/model.onnx`，重新
校验 manifest/role/graph/recipe/backend/range/source digest 和实际文件 SHA-256，命中后
仅复用 immutable model path；runner、KV、lease 与当前请求保持独立，protected grant
材料不进入 plaintext reuse。当前 closure 的 `spec185-provider-assembly` 重建通过；
5 个缓存/身份/保护域 C++ selectors 通过，包含篡改后删除精确 entry 和 independent
grant 隔离。首次 cold-hit selector 仅因未设置 `NDNSF_SPEC182_BIN_DIR` 找不到当前
assembly worker，修正环境变量后通过；该失败已记入 failure-log。证据见
[assembled-model cache verification](evidence/b189-assembled-model-cache-20260921.md)。
这只是 focused cache/static/build evidence，不推进 T003/T005/T006/T007/T009 或
最终 `QWEN_TWO_PROVIDER_PASS`；r203 的 Core timeout/runtime-unqualified 边界保持不变。
下一步是安装并核对当前 source closure 的 system binaries，再以新的 raw run ID 重跑
完整 MiniNDN production Repo 链路。

**B189 current-closure rebuild target-name preflight — 2026-09-21**: r203 后的 affected
native closure 构建命令在 Waf target 解析阶段失败，原因是把 executable 名称
`DI_NativeOnnxAssemblyWorker` 当作 target；没有编译、安装或运行变化。原始输出见
`.codex-tmp/spec189-current-closure-rebuild-20260921.log`；`./waf list` 已确认真实
target 为 `di-native-assembly-worker`。这是命令边界，不推进任何 checkbox；下一步使用
真实 target 重建当前 source closure，并核对 candidate receipt/source hash。

**B189 r203 model-materialization/Core-timeout boundary — 2026-09-21**: root MiniNDN
实际启动并完成两 Provider 的 ACK 前置路径。model-source cache hash 校验通过；cache 中
存在约 752 MB assembled stage-0 model，但 r203 Provider-0 日志没有
`NATIVE_SELECTION_ACCEPTED`、`ASSEMBLY_ADMISSION_REPORTED` 或 `stage=ASSEMBLY_STARTED`，
manifest 也没有本次 requestId，不能把文件无条件归因于 r203 的 post-Selection assembly，
更不证明 ORT runner load。Provider-1
没有对应 stage-1 assembled 文件；两 Provider 都只记录到
`GRANT_VERIFICATION boundary=BEFORE_ASSEMBLY`，没有 `EXECUTION_ENTERED`、
`ASSEMBLY_STARTED`、`RUNNER_READY`、ORT 或 terminal marker。Requester 首先报告
`NATIVE_REQUEST_TIMEOUT boundary=Core`，supervisor 为 `boundary=null`、`cleanup=PASS`、
无残留进程，资源门未停止。证据见 [r203 model materialization/Core timeout](evidence/b189-r203-model-materialization-core-timeout-20260921.md)。
保持 `RUNTIME_UNQUALIFIED`，不推进任何 checkbox；下一步先做 assembly/runner provenance
与 Core timeout 等待边界的静态审查，再以新 raw run 验证。

**B189 r202 root preflight boundary — 2026-09-21**: 首次尝试没有进入 MiniNDN；launcher
在 root preflight 停止并报告 `MININDN_REQUIRES_ROOT: run this script with sudo -E`。
supervisor cleanup 为 PASS，但没有 Controller/Requester/Provider 协议 marker，因此
不计作模型或协议结果。raw run 与首个边界见 [r202 root preflight](evidence/b189-r202-root-preflight-boundary-20260921.md)。
下一步使用相同 candidate、`sudo -n -E` 和新 raw run ID 重试；不推进任何 checkbox。

**B189 cleanup checkpoint — 2026-09-21**: 已在无活动 MiniNDN/Provider 进程的前提下
清理未被 durable evidence 引用的旧 run roots、失效 `external-*`/`encrypted-repo`
staging 和旧 SmolLM135M 大模型候选；保留当前 Qwen `identity-r1/canonical/stages/
candidate`、已引用 raw runs、历史 SmolLM runs/logs 及系统 model cache。根分区可用
空间约从 14 GiB 增至 40 GiB；证据见 [artifact cleanup](evidence/b189-artifact-cleanup-20260921.md)。
本 checkpoint 只维护实验工作区，不推进任何产品验收 checkbox。

**Explicit target refinement — 2026-09-21**: 本 Spec 的唯一验收目标已固定为
`MiniNDN + Qwen/Qwen3-0.6B + 2 execution Provider nodes (ucla, arizona) +
NDNSF-DI native C++ inference`。Controller/User/Repo 只计作支撑角色；stage export、
单 Provider、缓存命中、静态 selector 和 Python-only runner 不计目标完成。

**B189-4 native stage-materialization boundary checkpoint**: 2026-09-21 —
旧 Qwen stage 导出结果确认每个 stage 必须显式重建 graph input/output；当前
canonical template 的内部 handoff（例如 `hidden_states_out`）不在原始
canonical graph boundary 中。C++ `materializeNativeCanonicalModel` 已接收已认证
role contracts，按契约重建 stage I/O；新增
`MaterializedRoleRebuildsInternalStageBoundary` C++ selector 通过。受影响
native closure 和 `spec189-canonical-publisher` 编译通过；cold checker 的 external
initializer 问题随后已修复，同一 `Spec182CanonicalPublisher` 15-case regression
与 focused selector 均通过。受影响 native targets 已安装到 `/usr/local`，并核对了
hash、RUNPATH 与直接依赖；Waf 的 `py_repoclient` editable 安装仍因缺少
`NDNSF_GLOBAL_NATIVE_DIGESTS` 而失败，保留为安装/preflight gap。证据见
[stage materialization unit boundary](evidence/b189-stage-materialization-unit-20260921.md)。
本单元不推进任何产品验收 checkbox；下一步是确认 launcher 的 Python/native 加载闭包，
再以新 raw run ID 重跑完整 MiniNDN 两 Provider Qwen 链路。

**B189-4 fixed workspace and provider staging GC checkpoint**: 2026-09-21 —
默认 MiniNDN launcher 现在使用固定 `/var/tmp/ndnsf-di-spec189/ndnsf` 和
`repo` scratch；每次默认运行在 supervisor admission 后先清空并重建这两个目录，
保留 digest-namespaced `model-source/assembled` cache。显式 `--run-root` 仍完全
保留，作为 r189–r195 raw evidence 目录；固定 workspace 用 advisory lock 拒绝
并发清理。实际 standalone `di-native-provider` 在 plan load 前接入 staging GC，
只清理无 live lease 的过期 assembly/protected staging。6 项 Python 测试、Python
语法检查、C++ staging selector、受影响 Waf build 和 native install 通过；隔离
启动实测输出 `NDNSF_DI_PROVIDER_STAGING_GC removed=1`，并保留 `assembled/` 与
`model-source/`；launcher 预置旧 scratch 后以无效 manifest 失败时，旧 `ndnsf/`
和 `repo/` 被清理而 cache 保留。证据见 [fixed workspace/provider GC checkpoint](evidence/b189-fixed-workspace-provider-gc-20260921.md)。
这只是 cleanup/static/build focused boundary，不改变 r195 的
`NATIVE_STREAM_FAILED / stream event gap exceeded retry budget`，不关闭任何产品
验收 checkbox。安装仍有既有 `NDNSF_GLOBAL_NATIVE_DIGESTS` host binding 限制；下一步
在本次 static review 后，以新的显式 raw run root 重跑完整 MiniNDN 链路。

**B189-4 r195 stream-gap cache runtime boundary**: 2026-09-21 — r195 used the
installed candidate after the previous object cleanup. It reached both Provider
`GRANT_VERIFIED`/`EXECUTION_ENTERED`, Provider 0 `ASSEMBLY_STARTED`, and
Provider 1 `DEPENDENCY_FETCH`, then failed with
`NATIVE_STREAM_FAILED / stream event gap exceeded retry budget`. Cache-compatible
source and initializer references were returned and hash-verified with
`rootFetch=skipped` and `repoFetch=skipped`. No Provider reached `RUNNER_READY`,
ORT, terminal, or oracle. The supervisor reported `boundary=null`, cleanup
`PASS`, return code 1, and the resource sampler remained above its gates
(minimum free disk 4338700288 bytes). Evidence is in [r195 stream-gap cache
runtime boundary](evidence/b189-r195-stream-gap-cache-runtime-boundary-20260921.md).
This is `RUNTIME_UNQUALIFIED`; no checkbox advances. The next unit is the
reference-checked external/provider staging cleanup policy and focused failure
cleanup tests; it does not claim to resolve the r195 stream gap.

**B189-4 r194 status-query observation and host disk boundary**: 2026-09-21
— r194 ran the unchanged installed candidate with INFO logging only. The
requester recorded 150 `expressed`/`data-received`/`accepted` status queries;
both Providers contributed 75 accepted responses with the correct service,
digest, `state=Running`, and `members=2`. The run reached ACK/Selection,
Provider grant/execution, P0 `ASSEMBLY_STARTED`, and P1 dependency-fetch begin,
then the supervisor stopped it at `RESOURCE_BOUNDARY:diskFree` (minimum
4200099840 bytes against the 4294967296-byte gate). Cleanup passed, no process
remained, and memory/swap gates did not trigger. No Provider reached
`RUNNER_READY`, ORT, terminal, or oracle. Evidence is in [r194 status-query
observation and disk boundary](evidence/b189-r194-status-query-observe-disk-boundary-20260921.md).
This is `RUNTIME_UNQUALIFIED`; no checkbox advances. Release disk only through
an explicit, reference-checked cleanup, then rerun the same candidate and keep
the static review/build/runtime gate sequence intact.

**B189-4 r193 delayed status-query runtime boundary**: 2026-09-21 — r193
passed ACK/Selection, both Provider grant/execution entries, P0
`ASSEMBLY_STARTED`, and P1 `DEPENDENCY_FETCH` begin, then failed with
`NATIVE_STREAM_FAILED / stream event gap exceeded retry budget`. The supervisor
reported `boundary=null`, cleanup `PASS`, no remaining processes, and no
resource stop; no Provider reached `RUNNER_READY`, ORT, terminal, or oracle.
The cache-compatible source and initializer were returned and hash-verified
with `rootFetch=skipped` and `repoFetch=skipped`. No status-query marker was
visible under the run's WARN log filter, so the delayed-query change is not
runtime-proven. Evidence is in [r193 status-query delay boundary](evidence/b189-r193-status-query-delay-runtime-boundary-20260921.md).
This is `RUNTIME_UNQUALIFIED`; no checkbox advances. Before another run,
perform the required immutable post-failure static review and prove the
initial-query scheduling boundary with a focused assertion or runtime marker.

**B189-4 r192 status-query race runtime boundary**: 2026-09-21 — r192
passed ACK/Selection, both Provider grant/execution entries, P0
`ASSEMBLY_STARTED`, and P1 `DEPENDENCY_FETCH` begin. The supervisor remained
clear (`boundary=null`, cleanup `PASS`); minimum disk, memory, and swap stayed
above the gates. The requester still failed with
`NATIVE_STREAM_FAILED / stream event gap exceeded retry budget`, with no
`RUNNER_READY`, ORT, terminal, or oracle result. Both initial Selection-status
queries timed out. Static review identified the immediate-query race: the
Provider status record is created while processing Selection, after the User
publishes Selection and starts the first query. The next changed gate defers
that first query by the configured status interval, then preserves bounded
polling. Evidence is in [r192 status-query race boundary](evidence/b189-r192-status-query-race-runtime-boundary-20260921.md).
This is `RUNTIME_UNQUALIFIED`; no checkbox advances. Take the required
post-failure immutable static snapshot before the next build or MiniNDN run.

**B189-4 r191 host resource boundary**: 2026-09-21 — after the Core
status-cache repair was installed, r191 reached requester Selection commit,
both Provider grant/execution entries, and P0 `ASSEMBLY_STARTED`. The host
supervisor then stopped the run at `RESOURCE_BOUNDARY:diskFree`: minimum free
disk was 3659636736 bytes against the 4294967296-byte gate. Cleanup passed,
memory/swap gates did not trigger, and no stream, native failure, ORT, or
terminal result is inferred. Evidence is in
[r191 host disk boundary](evidence/b189-r191-host-disk-boundary-20260921.md).
Old unreferenced `external-r127`, `external-r129`, and `external-r131`
staging directories were removed after reference checks; current
canonical/stage/identity material, referenced staging, and r189–r191 raw runs
are retained. Free disk is now about 7.7 GiB, above the 4 GiB gate. This is
`UNQUALIFIED`; no checkbox advances. Rerun the same MiniNDN candidate and then
perform the required post-run static review.

**B189-4 r190 stream-gap/status-cache runtime boundary and next static gate**:
2026-09-21 — r190 passed ACK/Selection, assignment selection, both Provider
grant/execution entries, P0 `ASSEMBLY_STARTED`, and P1 dependency-fetch begin,
but the requester exhausted the stream event-gap retry budget while P0 was
still assembling. No native failure or output-contract marker was observed;
cleanup passed and all resource guards remained clear. The run also showed
unbound Selection-status replies with `payloadService=/`, so the existing
authenticated assembly heartbeat could not reset the stream cursor. The
changed Core path now suppresses unregistered negative status Data rather than
caching an invalid binding; focused C++ build/test passed. Evidence is in
[r190 stream-gap/status-cache boundary](evidence/b189-r190-stream-gap-status-cache-boundary-20260921.md).
This is `RUNTIME_UNQUALIFIED`; no checkbox advances. Before r191, perform a
fresh immutable static review of this changed Core path, rebuild/install the
affected framework and DI targets, then rerun the same Qwen flow.

**B189-4 r190 passthrough-aware output-alias static gate**: 2026-09-21 —
static review confirmed that Qwen's `attention_mask` and `position_ids` are
authenticated role outputs carried as local ONNX passthrough inputs, not
additional activation outputs. Cold assembly and final preparation now filter
those names from the alias candidate set, require a single semantic activation
edge with a single concrete non-KV output, and fail closed on ambiguity. The
immutable snapshot `.codex-tmp/spec189-r190-passthrough-alias-review-v1/`
passed diff, filter-source, cardinality, both-call-site, and no-detach checks.
The focused C++ target built and all four preparation-context tests passed;
aggregate `unit-tests` remains blocked by unrelated old aggregate-assignment
errors in `di-native-planning.t.cpp`. Evidence is in
[r190 passthrough-alias static review](evidence/b189-r190-passthrough-alias-static-review-20260921.md).
This `STATIC_PASS` releases the affected install and one MiniNDN runtime; no
checkbox advances.

**B189-4 r189 preparation-alias runtime boundary**: 2026-09-21 — the
installed preparation-alias candidate completed cache-compatible source
verification, ACK/Selection, both Provider grant/execution boundaries, and
Provider-0 `RUNNER_READY` plus ONNX execution. It failed at the unchanged
first production boundary while selecting the inter-Provider tensor:
`tensor bundle has no tensor: hidden-layer-13-to-14`. Provider-1 reached
dependency-fetch begin; cleanup passed, the resource guard did not stop the
run, and no terminal/oracle result exists. The failure is caused by treating
the Qwen passthrough outputs `attention_mask` and `position_ids` as additional
activation outputs, so the 1:1 alias helper silently returns. Evidence is in
[r189 preparation-alias runtime boundary](evidence/b189-r189-preparation-alias-runtime-boundary-20260921.md).
No checkbox advances. The next gate is the focused C++ passthrough-aware alias
repair, followed by regression assertion, static review, affected build, and
the same MiniNDN run.

**B189-4 r189 preparation-alias static gate**: 2026-09-21 — after r188's
post-`RUNNER_READY` output-edge failure, the final
`bindNativeRunnerPreparationContext` path now reprojects authenticated local
role outputs and binds concrete ONNX activation outputs to semantic edge IDs
after local KV names are known. The immutable snapshot
`.codex-tmp/spec189-r189-preparation-alias-review-v1/` passed diff, source,
state-exclusion, cardinality, no-model-family-gate, and no-lifecycle-change
checks. Evidence is in
[r189 preparation-alias static review](evidence/b189-r189-preparation-alias-static-review-20260921.md).
This `STATIC_PASS` releases the affected build/install and one runtime attempt;
no checkbox advances.

**B189-4 r189 affected native build/install**: 2026-09-21 — the focused
`ndnsf-distributed-inference`, `di-native-provider`, and `DI_NativeRequester`
build passed with `-j4`, and the affected native system install returned `0`.
The staged `py_repoclient` editable install retained the existing
`NDNSF_GLOBAL_NATIVE_DIGESTS` limitation, while the native binaries required
by MiniNDN were installed. Evidence is in
[r189 preparation-alias build](evidence/b189-r189-preparation-alias-build-20260921.md).
This is `COMPILE_LINK_PASS` for the affected native boundary only; no checkbox
advances.

**B189-4 r188 output-alias preparation boundary**: 2026-09-21 — the
cache-compatible Qwen 0.6B two-Provider run passed source-cache verification
(`rootFetch=skipped`, `repoFetch=skipped`), Selection, both grant-verification
boundaries, and Provider-0 `ASSEMBLY_STARTED` → `RUNNER_READY`; Provider-1
reached dependency-fetch begin. Provider-0 then failed while publishing the
authenticated inter-Provider edge with `tensor bundle has no tensor:
hidden-layer-13-to-14`. This is after runner construction, so r186's assembler
alias repair is insufficient as the final preparation-context closure. The
run retained `supervisor cleanup=PASS`, no remaining processes, and 252 resource
samples above the stop gates. Evidence is in
[r188 output-alias preparation boundary](evidence/b189-r188-output-alias-preparation-boundary-20260921.md).
The next changed gate is a focused C++ preparation-context repair, then static
review, affected build/install, and a new MiniNDN run; no checkbox advances.

**B189-4 r186 output-alias repair static gate**: 2026-09-21 — the repair no
longer gates concrete-output-to-semantic-edge alias binding on the
assembly-time `projection.plan.modelFamily`; authenticated role outputs and
concrete expected outputs remain the source of truth. Terminal override, KV
state exclusion, one-to-one cardinality, cold/cache call sites, diagnostic
gate, and no-detach checks all passed on immutable snapshot
`.codex-tmp/spec189-r186-output-alias-review-v1/`. Evidence is in
[r186 output-alias static review](evidence/b189-r186-output-alias-static-review-20260921.md).
This `STATIC_PASS` releases the affected build and one runtime attempt; the
runtime result must be followed by another static review. No checkbox advances.

**B189-4 r185 concrete output alias remained unbound**: 2026-09-21 — the
valid-log cache-compatible run passed source-cache skip, Selection, both
Provider grant/execution boundaries, and Provider-0 `RUNNER_READY`; Provider-1
reached dependency-fetch begin. The P0 manifest still published concrete
`qwen_s0_hidden_states_out` without `outputAlias.*`, then failed before ORT
`Run` with missing semantic tensor `hidden-layer-13-to-14`, so the output
contract diagnostic marker was not reached. Cleanup PASS and resource gates
were preserved. Evidence is in
[r185 output-alias runtime boundary](evidence/b189-r185-output-alias-runtime-boundary-20260921.md).
The next changed gate removes the assembly-time `modelFamily` dependency from
alias binding, followed by static review, build, and runtime; no checkbox
advances.

**B189-4 r184 selection-status gap before ORT**: 2026-09-21 — the Qwen
cache-compatible run reached both Provider ACK and grant-verification
boundaries, with `repoFetch=skipped-after-selection` on both providers, but
the requester never recorded Selection commit or assignment selection and
ended with `NATIVE_STREAM_FAILED` after a stream event gap. No dependency
fetch, assembly, runner, or ORT output-contract evidence exists. Cleanup was
PASS with no remaining processes; 234 resource samples stayed above the
available-memory and disk gates. Evidence is in
[r184 selection-status gap boundary](evidence/b189-r184-selection-status-gap-boundary-20260921.md).
The next run only restores the known-valid `NDNSF_NDN_LOG='*=WARN'` diagnostic
configuration; no task checkbox advances.

**B189-4 r183 invalid NDN logging configuration boundary**: 2026-09-21 — the
manifest preflight passed, then bootstrap `ndnsec key-gen` aborted because the
launch command set `NDNSF_NDN_LOG=info`, which became invalid `NDN_LOG=info`
(`malformed logging config: '=' is missing`). A same-environment minimal
reproduction succeeded after removing that variable. No ACK, Selection,
Provider, or ORT evidence exists; the raw run root and launch log are retained.
Evidence is in
[r183 NDN log preflight boundary](evidence/b189-r183-ndn-log-preflight-boundary-20260921.md).
The next retry only removes this logging variable; no task checkbox advances.

**B189-4 r182 EOS manifest preflight boundary**: 2026-09-21 — the corrected
manifest passed the explicit `modelFamily` check, then startup stopped because
`stage-manifest-qwen-r99.json` lacks `eosTokenIds` and raised
`MODEL_EOS_TOKEN_IDS_REQUIRED`. No MiniNDN, ACK, Selection, Provider, or ORT
evidence exists; the raw run root and launch log are retained. The sibling
`stage-manifest-qwen-v2.json` has the same Qwen revision and layer ranges plus
`eosTokenIds: [151645]`. Evidence is in
[r182 EOS preflight boundary](evidence/b189-r182-eos-preflight-boundary-20260921.md).
The next retry only switches to that manifest and recomputes its hash; no task
checkbox advances.

**B189-4 r181 stage-manifest preflight boundary**: 2026-09-21 — the first
r181 launch stopped before MiniNDN because the selected legacy
`candidate/stage-manifest.json` lacks explicit `modelFamily`. The raw run root
and launch log are retained; no ACK, Selection, Provider, or ORT evidence was
created. The sibling `stage-manifest-qwen-r99.json` and
`stage-manifest-qwen-v2.json` both declare `modelFamily: qwen` with layer ranges
`[0,14)` and `[14,28)`. Evidence is in
[r181 stage-manifest preflight boundary](evidence/b189-r181-stage-manifest-preflight-boundary-20260921.md).
The next retry only corrects the manifest input and recomputes its hash; no
task checkbox advances.

**B189-4 r180 output-contract diagnostic static gate**: 2026-09-21 — the
trace-gated C++ diagnostic records the actual ORT output names and the existing
alias lookup result without recording tensor payload, KV state, or input. The
immutable snapshot `.codex-tmp/spec189-r180-output-contract-diagnostic-review-v1/`
passed the read-only review, `git diff --check`, trace-gate, actual-output-name,
alias-lookup, no-payload, and no-lifecycle-change checks. Evidence is in
[r180 output-contract diagnostic static review](evidence/b189-r180-output-contract-diagnostic-static-review-20260921.md).
This releases one diagnostic build and run only; no task checkbox advances.

**B189-4 r180 output-alias repair reached the same concrete boundary**:
2026-09-21 — the installed alias candidate passed cache-compatible preflight
with `rootFetch=skipped repoFetch=skipped`, authenticated ACK/Selection, both
Provider grant/execution-entered boundaries, and Provider-0 `RUNNER_READY`.
It then failed at the unchanged first production boundary
`tensor bundle has no tensor: hidden-layer-13-to-14`; Provider-1 reached
dependency-fetch begin and cleanup passed. Resource limits were unchanged
(minimum available `3455885312`, minimum disk `5393469440`, maximum sampled
aggregate RSS `6242652160`, maximum owned swap `5087232`). The canonical graph
inspection binds the concrete layer-13→14 tensor to `/Add_4_output_0`; the
next repair must correct assembled ONNX I/O construction rather than repeat
metadata-only aliasing. Evidence is in
[r180 output-alias boundary](evidence/b189-r180-output-alias-repair-boundary-20260921.md).
T003-R4 remains `RUNTIME_OUTPUT_ALIAS_BOUNDARY`; T003/T005/T006/T007/T009
remain `PARTIAL`; no checkbox is advanced.

**B189-4 r179 output-alias repair static pass**: 2026-09-21 — the Qwen-only
non-terminal activation repair maps concrete prepared ONNX output names to the
authenticated semantic edge tensor IDs, excludes KV state outputs, requires
one-to-one cardinality, and retains the terminal `final-response` path. The
immutable snapshot `.codex-tmp/spec189-r179-output-alias-review-v1/` passed
read-only review, `git diff --check`, Qwen/terminal ordering, KV exclusion,
alias cardinality/source, both cold/cache call-site, and no-detach checks.
Evidence is in
[r179 output-alias static review](evidence/b189-r179-output-alias-static-review-20260921.md).
The affected native build and r180 runtime are released; no task checkbox is
advanced.

**B189-4 r179 output-scope repair reached RUNNER_READY, then output-alias boundary**: 2026-09-21 — the installed candidate passed cache-compatible
preflight with `rootFetch=skipped repoFetch=skipped`, authenticated
ACK/Selection, both Provider grant/execution-entered boundaries, and
Provider-0 `RUNNER_READY`; the r178 missing-scope error did not recur. The
next first boundary was the missing semantic tensor
`hidden-layer-13-to-14` because the assembled ONNX output is named
`qwen_s0_hidden_states_out`. Provider-1 reached dependency-fetch begin;
cleanup passed and no terminal/oracle result exists. Evidence is in
[r179 output-alias boundary](evidence/b189-r179-output-alias-boundary-20260921.md).
T003-R4 is `RUNTIME_OUTPUT_ALIAS_BOUNDARY`; T003/T005/T006/T007/T009 remain
`PARTIAL`. The next gate is authenticated Qwen output-alias binding, then a
fresh static review before r180.

**B189-4 r178 output-scope repair static pass**: 2026-09-21 — the repair
reuses the production role selector's exact runtime scope in both cold and
assembled-cache runner specs, while retaining the terminal `final-response`
override. The immutable snapshot
`.codex-tmp/spec189-r178-output-scope-review-v1/` passed the read-only review,
`git diff --check`, selector/call-site invariants, cache/cold coverage, and
no-detach checks. Evidence is in
[r178 output-scope static review](evidence/b189-r178-output-scope-static-review-20260921.md).
The affected native build and r179 runtime are released; no task checkbox is
advanced.

**B189-4 r178 cache-memory repair reached RUNNER_READY, then output-scope boundary**: 2026-09-21 — the installed cache-compatible candidate passed
source-cache preflight with `rootFetch=skipped repoFetch=skipped`, authenticated
ACK/Selection, both Provider grant/execution-entered boundaries, and the
memory repair reduced OA02 worker RSS to about 2.7 GiB. Provider-0 reached
`RUNNER_READY` but failed before terminal publication because its runner spec
did not publish the authenticated dataflow scope. Provider-1 reached only
`DEPENDENCY_FETCH status=begin`; cleanup passed and no execution, terminal, or
oracle result exists. Evidence is in
[r178 output-scope boundary](evidence/b189-r178-cache-memory-repair-output-scope-boundary-20260921.md).
T003-R4 is `RUNTIME_OUTPUT_SCOPE_BOUNDARY`; T003/T005/T006/T007/T009 remain
`PARTIAL`. The next gate is a focused output-scope repair in both cold and
assembled-cache runner specs, followed by a fresh static review before build
and r179 runtime.

**B189-4 r177 memory repair post-compile-fix static pass**: 2026-09-21 — the
one-argument compile repair was re-reviewed on immutable snapshot
`.codex-tmp/spec189-r177-memory-review-v3/`; no P0-P3 issue was found. The
signature check, source-lifetime/materialization invariants, worker reap
invariant, and `git diff --check` passed. Evidence is in
[r177 memory static review v3](evidence/b189-r177-memory-static-review-v3-20260921.md).
The affected build retry is released; no runtime status changed.

**B189-4 r177 memory repair compile boundary**: 2026-09-21 — the first
affected-target build stopped before linking because the new
`materializeShapeInferenceInitializers` call omitted its declared
`materializedBudget` argument. No runtime was started and no task is complete.
The failure is preserved in [failure-log](../../docs/failure-log.md); the
minimal call-site repair must pass a fresh static check before the build retry.

**B189-4 r177 cache-worker memory repair: static pass**: 2026-09-21 — the
repair keeps the authenticated source graph external, hashes its verified cache
bytes directly, materializes only bounded shape inputs and selected-role
initializers, and transfers selected TensorProto ownership with `Swap()` before
scrubbing the worker source. The read-only snapshot review found no P0-P3
issue; `git diff --check` and focused source-lifetime/materialization
invariants passed. Cache identity, Selection, protected-runtime, resource
limits, and worker join/cancel behavior remain unchanged. Evidence is in
[r177 memory static review](evidence/b189-r177-memory-static-review-20260921.md).
An incremental native build and fresh runtime are now the only released gates;
r177 remains `UNQUALIFIED / RESOURCE_BOUNDARY`.

**B189-4 r177 heartbeat runtime reached worker, then memory boundary**:
2026-09-21 — the installed heartbeat candidate passed cache preflight,
authenticated ACK/Selection, both Provider grant/execution-entered
boundaries, and Provider-0 verified the cached source and initializer while
logging `rootFetch=skipped repoFetch=skipped`. No stream gap recurred. The
unchanged host guard then stopped the run at
`RESOURCE_BOUNDARY:MemAvailable` (`1545871360` below the
`1610612736` floor); the OA02 assembly worker peaked at about 5.7 GiB RSS.
No runner, terminal response, or oracle result exists; cleanup passed. The
run is preserved in
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r177-assembly-heartbeat/`
and detailed in
[b189-r177 evidence](evidence/b189-r177-cache-bypass-memory-boundary-20260921.md).
T003-R4 remains `RUNTIME_RESOURCE_BOUNDARY`; the next gate is a focused
native worker/cache memory-ownership repair followed by static review.

**B189-4 r176 stream-liveness heartbeat repair: static pass**: 2026-09-21 —
the changed gate adds a join-scoped authenticated assembly-progress heartbeat
around the synchronous OA02 worker, preserving the worker's first exception
and all existing cache, protected-runtime, Selection, and resource gates. The
read-only review found no P0-P3 issue; evidence is preserved in
[heartbeat static review](evidence/b189-r176-heartbeat-static-review-20260921.md).
`git diff --check`, Python compilation, and the focused heartbeat invariant
check passed. Native rebuild and the fresh trace-enabled runtime remain
pending; r176 itself remains `UNQUALIFIED / RUNTIME_STREAM_GAP`.

**B189-4 r176 cache bypass reached verified material, then stream gap**:
2026-09-21 — the latest installed candidate passed source-cache preflight,
authenticated ACK/Selection, both Provider grant/execution-entered boundaries,
and Provider-0 explicitly logged `rootFetch=skipped repoFetch=skipped`. The
cache-backed source and 1.5 GiB initializer both returned and passed digest
verification. No runner, terminal response, or oracle result followed;
requester Core failed with `stream event gap exceeded retry budget` before
runner activation. Evidence is preserved in
[b189-r176 evidence](evidence/b189-r176-cache-bypass-stream-gap-boundary-20260921.md).
The guard did not stop the run: 156 samples recorded minimum disk free
`7267594240`, minimum available memory `3762192384`, and maximum owned swap
`74481664`; cleanup passed. T003-R4 is now
`RUNTIME_STREAM_GAP / STATIC_PENDING_DIAGNOSIS`; T003/T005/T006/T007/T009
remain `PARTIAL`. The next changed gate is trace-only observation of signed
Selection-status/progress delivery during assembly, followed by a fresh static
review before any code repair.

**B189-4 r175 disk guard after model-name repair**: 2026-09-21 — the repaired
and installed candidate passed source-cache preflight, but MiniNDN startup was
stopped by `RESOURCE_BOUNDARY:diskFree` before ACK/Selection. Evidence is
preserved in `evidence/b189-r175-disk-guard-after-model-name-repair-20260921.md`;
74 samples recorded minimum disk free `4234629120`, below the maintained 4 GiB
floor, while minimum available memory was `6448869376` and maximum owned swap
was `5468160`. Cleanup passed with no remaining process. T003-R4 remains
`RUNTIME_PENDING_RESOURCE` after the code repair; T003/T005/T006/T007/T009
remain `PARTIAL`.

**B189-4 r174 cache model-name domain boundary**: 2026-09-21 — the latest
cache-compatible run passed source-cache preflight, authenticated ACK/Selection,
both Provider grants, and both `EXECUTION_ENTERED` boundaries. Provider-0 then
failed at `ASSEMBLY_STARTED` with
`DI_CACHE_COMPATIBILITY_IDENTITY_MISMATCH`: the cache stores external model
name `Qwen/Qwen3-0.6B`, while the sealed plan uses control-plane URI
`/Model/Qwen3/0.6B`. Evidence is preserved in
`evidence/b189-r174-cache-model-name-domain-boundary-20260921.md`. The
minimal repair removes only this cross-domain string comparison, keeps graph,
source/initializer digest and object verification, and passed `git diff --check`,
Python compilation, the cache invariant check, and CodeGraph inspection;
affected build and runtime retry remain pending. T003-R4 is
`IMPLEMENTED / STATIC_PASS / BUILD_PENDING`; T003/T005/T006/T007/T009 remain
`PARTIAL`.

**B189-4 r173 disk guard boundary**: 2026-09-21 — the cache-compatible
candidate passed source-cache preflight, but MiniNDN startup was stopped by
`RESOURCE_BOUNDARY:diskFree`; minimum free disk was `4219887616`, below the
4 GiB guard. Evidence is preserved in
`evidence/b189-r173-disk-guard-boundary-20260921.md`. No protocol or model
result is claimed; T003-R4 remains `RUNTIME_PENDING_RESOURCE`; T003/T005/T006/T007/T009 remain `PARTIAL`.

**B189-4 r172 cache identity contract boundary**: 2026-09-21 — after the
protected-role repair, r172 reached verified cache, authenticated ACK/Selection,
both Provider grants, and Provider-0 `ASSEMBLY_STARTED`; it then stopped at
`DI_CACHE_COMPATIBILITY_IDENTITY_MISMATCH`. The cache preparation-stage
manifest digest was incorrectly compared with the selected publication-root
receipt digest. Evidence is preserved in
`evidence/b189-r172-cache-identity-contract-boundary-20260921.md`. The next
repair separates these digest domains; T003-R4 is `RUNTIME_BUG / STATIC_PENDING`;
T003/T005/T006/T007/T009 remain `PARTIAL`.

**B189-4 r171 protected-role compatibility boundary**: 2026-09-20 — the
verified cache and authenticated ACK/Selection reached both Provider grant
checks and Provider-0 `ASSEMBLY_STARTED`; the assembler then rejected the
protected role with `DI_CACHE_COMPATIBILITY_PROTECTED_UNSUPPORTED`. The
failure is recorded in
`evidence/b189-r171-cache-protected-role-boundary-20260920.md`. The repair
removes only that over-broad rejection; protected runtime/content-key and
secure-erase checks remain. T003-R4 is `IMPLEMENTED / STATIC_PENDING` until
the repair review and affected rebuild pass; T003/T005/T006/T007/T009 remain
`PARTIAL`.

**B189-4 r170 bootstrap logging boundary**: 2026-09-20 — the Qwen v2
manifest and verified source cache passed preflight, then bootstrap stopped at
`ndnsec key-gen` because the invocation used invalid `NDNSF_NDN_LOG=0`.
Evidence is preserved in
`evidence/b189-r170-bootstrap-logging-boundary-20260920.md`; the next retry
uses `NDNSF_NDN_LOG='*=WARN'`. T003-R4 remains
`STATIC_PASS / BUILD_PASS / RUNTIME_PENDING`; T003/T005/T006/T007/T009 remain
`PARTIAL`.

**B189-4 r169 manifest contract boundary**: 2026-09-20 — the corrected
invocation passed argument parsing and `modelFamily=qwen`, but the selected
`stage-manifest-qwen-r99.json` lacked Qwen `eosTokenIds`, so preflight stopped
with `MODEL_EOS_TOKEN_IDS_REQUIRED`. Evidence is preserved in
`evidence/b189-r169-stage-manifest-contract-boundary-20260920.md`; the next
retry uses `stage-manifest-qwen-v2.json`. T003-R4 remains
`STATIC_PASS / BUILD_PASS / RUNTIME_PENDING`; T003/T005/T006/T007/T009 remain
`PARTIAL`.

**B189-4 r168 launcher argument boundary**: 2026-09-20 — the valid Qwen
manifest reached the launcher, but the retry stopped at argument parsing because
the wrapper used `--assembly-worker-sha256` rather than the maintained
`--assembly-worker-binary-sha256`. This is an invocation-only failure; raw
evidence is preserved in
`evidence/b189-r168-launch-argument-boundary-20260920.md`. T003-R4 remains
`STATIC_PASS / BUILD_PASS / RUNTIME_PENDING`; T003/T005/T006/T007/T009 remain
`PARTIAL`.

**B189-4 r167 manifest preflight boundary**: 2026-09-20 — the first r167
cache-compatible retry stopped before MiniNDN/NFD because the selected
`candidate/stage-manifest.json` lacked the required explicit `modelFamily`.
This is a run-input failure, not a protocol or resource result; the raw
launcher output is preserved in
`evidence/b189-r167-stage-manifest-preflight-boundary-20260920.md` and the
next retry uses the existing `stage-manifest-qwen-r99.json`. T003-R4 remains
`STATIC_PASS / BUILD_PASS / RUNTIME_PENDING`; T003/T005/T006/T007/T009 remain
`PARTIAL`.

**B189-4 T003-R4 cache-compatible root prefetch bypass**: 2026-09-20 23:39 -0500 —
r166 crossed authenticated ACK/Selection but Core still prefetched the metadata-only
compatibility root and failed with `Nack Error` before Provider assembly. The repair
adds an explicit `artifactPrefetchRequired` binding: ordinary publications retain
the default prefetch, while the authenticated compatibility receipt keeps
`assignedArtifact` and omits `artifactDataName`; Provider-side cache identity and
assembly remain the next boundary. Static review and affected build are pending;
T003/T005/T006/T007/T009 remain `PARTIAL`. Raw r166 is preserved at
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r166-cache-compatibility-requester/`.

**B189-3 r139 authenticated-Selection disk-free boundary**: 2026-09-20 — r139
used a fresh run/external root, the repaired launcher, the exact installed
candidate, unchanged resource/stream limits, and `NDNSF_NDN_LOG='*=TRACE'`.
The unchanged host guard stopped at `RESOURCE_BOUNDARY:diskFree`; cleanup
passed. The 315-sample record reached minimum available memory
`2534084608`, minimum disk free `4123873280`, aggregate RSS peak
`8117018624`, owned-swap peak `156700672`, and swap-I/O delta `458199040`.
The requester emitted `NDNSF_DI_NATIVE_ACK_CLOSED` and
`NDNSF_DI_NATIVE_SELECTION_COMMITTED`; both Providers accepted authenticated
Selection and recorded `GRANT_VERIFICATION` at `BEFORE_ASSEMBLY`. Provider-0
entered placement-bound dependency preparation, reported assembly admission,
and emitted `ASSEMBLY_STARTED`; both Providers emitted `EXECUTION_ENTERED`.
The requester received signed `SELECTION_STATUS_QUERY` replies from both
Providers. Provider-0 retained a `752097499`-byte staging `canonical.onnx`
and a `752308868`-byte protected assembly cipher. No `RUNNER_READY`,
`EXECUTION_COMPLETED`, terminal response, second request, or C++ oracle result
exists. The request cancellation is a consequence of the guard stop, not a
protocol rejection. Raw output is `.codex-tmp/spec189-r139-launch.log`; raw
run root is `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r139/`.
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is
bounded static/runtime review of the r139 materialization and resource-owner
boundary, followed by a fresh guarded run without changing limits or deleting
raw evidence.

After r139, its preserved Provider-0 staging `canonical.onnx` was independently
verified as digest
`sha256:258f367628b69a66eccf0973386b2eb80e2fce74cd64c2172d4f00a6e06abb94`
and atomically hard-linked to canonical staging inode `4853426`; the r139 path
and bytes remain, the inode has `11` links, and root ext4 free space is
`4875534336` bytes. The unique protected ciphertext and raw logs were not
changed. This is space-preserving evidence maintenance only; no task status
changes.

The first read-only re-review of the r139 Changed gate used immutable snapshot
`.codex-tmp/spec189-r139-source-staging-review-v1/` and returned
`NOT_STATIC_PASS`: the snapshot omitted `NativeModelRunner.hpp`, the protected
cleanup catch could mask the first assembly error, and the artifact/cache owner
handoff plus cleanup-failure evidence needed repair. No rebuild or rerun was
performed. The bounded repair preserves the first exception, transfers the
protected artifact-directory lifetime through `NativeModelRunnerSpec` into the
cache cleanup callback, and emits
`NDNSF_DI_PROVIDER_ARTIFACT_CLEANUP_FAILED`; a corrected immutable snapshot
and read-only review are required before build.

The second read-only re-review used immutable snapshot
`.codex-tmp/spec189-r139-source-staging-review-v2/` and found one P1: the
protected success path used `std::filesystem::remove(sourceFile)`, bypassing
the pinned-directory fd's overwrite/fsync/unlink cleanup. No build or rerun was
performed. The repair now returns a file eraser bound to the registered
directory lease fd, reuses the same secure entry cleanup for early
`canonical.onnx` release, rejects non-direct children, and preserves the
stable source-staging failure marker. A new immutable snapshot and read-only
review are required before the affected build.

The third read-only re-review used immutable snapshot
`.codex-tmp/spec189-r139-source-staging-review-v3/` and found a P1 race between
the returned early eraser and the final runtime lease drain, plus a P2 in the
range-backed template/node validation path that passed `data()` (which is null
for a range source) to protobuf. No build or rerun was performed. The repair
serializes early erase and final drain with one directory-lease mutex and
materializes validation views through `copyBytes()`. A new immutable snapshot
and read-only review are required before the affected build.

The fourth immutable snapshot
`.codex-tmp/spec189-r139-source-staging-review-v4/` passed the read-only review
with `STATIC_PASS`: all 20 current-file hashes and the diff hash matched
`base-head` `78e1a4ca`, and no actionable P0-P3 finding remained. The review did
not build, install, run MiniNDN, or qualify Spec189. The affected C++ build and
focused selectors are now the next gate.

**Evidence maintenance after push**: 2026-09-20 — after checkpoint
`d13d6045` was pushed to `origin/Experimental`, 104 old run-scoped
`encrypted-repo`, `canonical-repo`, and Provider cache directories were removed
from the local `.codex-tmp` workspace. The removed payload/cache set was about
`41 GiB`; old run logs, JSON, certificates, and resource samples were retained,
and the complete r139 run root was retained at
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r139/`.
The remaining Spec189 runs directory is about `4.9 GiB`, and root free space
rose from about `4.1 GiB` to `42 GiB`. This is local evidence maintenance only:
no protocol result or task checkbox changes, T005/T006/T007/T009 remain
`PARTIAL`, and no Codex conversation files or Git refs were modified.
Details are in [run artifact cleanup evidence](evidence/b189-run-artifact-cleanup-20260920.md).

**B189-3 r119 range-source focused-check boundary**: 2026-09-20 — the first
focused selector invocation after the bounded publication repair used the
wrong worker fixture directory (`/usr/local/bin` instead of
`build-spec189-oracle`), so the material-consumer and ONNX activation cases
stopped before their assertion bodies. The Repo publication case did execute
and found a caller mismatch: external initializer range-view payloads were
still measured and written through `MaterialPayload::bytes.size()`, producing
`repo-publication-identity-mismatch`. The repair now adds a verified Repo
range-backed source, bounded `copyBytes()` publication, and `byteSize()` Repo
metadata checks. Raw selector logs are
`.codex-tmp/spec189-r119-material-selector.log` and
`.codex-tmp/spec189-r119-onnx-repo-selector.log`; durable detail is in
[r119 focused-check evidence](evidence/b189-r119-range-source-focused-check-20260920.md).
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is
to rebuild the affected targets, rerun with the explicit build fixture
directory, and review the resulting selectors before installation.

The follow-up rebuild passed `434/434`; with the correct fixture directory the
material consumer selector passed `2/2` and the ONNX selectors entered their
assertion bodies. The Repo publication selector then exposed two further
source gaps: strict `getRangeIfCurrent()` identity did not match the persisted
manifest representation, and external initializer selection fetched only the
header instead of all authenticated `chunkPayloadIds`. No checkbox changes;
T005/T006/T007/T009 remain `PARTIAL`. Raw output is
`.codex-tmp/spec189-r119-material-selector-v2.log` and
`.codex-tmp/spec189-r119-onnx-repo-selector-v2.log`; the same durable r119
evidence records this follow-up. Next step is to repair these two C++ Repo
selection boundaries, rebuild, and rerun the same selectors.

That rerun passed the material consumer `2/2`, the ONNX extraction/assembly/
activation selectors, and the Repo source-range check, but the Repo
post-Selection case stopped at `selected material payload differs from
manifest reference`: chunk IDs are authenticated by a shared-initializer
reference, while their individual digest/size are in the root's authenticated
`materialObjects` record. No checkbox changes; T005/T006/T007/T009 remain
`PARTIAL`. Next step is to bind chunk selection to its owning authenticated
reference, rebuild, and rerun the same C++ selectors.

The r119 repair then passed the affected native build `434/434`. The material
consumer selector passed `2/2`; the combined ONNX extraction, assembly,
activation, and Repo publication selectors completed with `*** No errors
detected`, including bounded Repo range reads and post-Selection chunk
binding. These are focused C++ checks only: no task checkbox changes;
T005/T006/T007/T009 remain `PARTIAL`. Raw output is
`.codex-tmp/spec189-r119-material-selector-v4.log`,
`.codex-tmp/spec189-r119-onnx-repo-selector-v4.log`, and
`.codex-tmp/spec189-r119-build-range-source-v4.log`. Next step is static diff
review, exact installed-runtime rebuild/installation, loaded-path/hash
verification, and a fresh guarded MiniNDN request-chain run.

The first fresh installed-runtime attempt was stopped before MiniNDN at a
launcher invocation boundary: `/usr/bin/python3` under `sudo env` could not
import the user-installed `ndn` package, so provider key-prefix decoding was
skipped even though the preserved PIB contained the provider key. No checkbox
changes; T005/T006/T007/T009 remain `PARTIAL`. Raw output is
`.codex-tmp/spec189-r120-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r120/`.
Next step is a fresh run root with the same candidate and explicit digests,
plus the verified user-site `PYTHONPATH`.

The r121 retry used that explicit `PYTHONPATH` and the installed candidate. It
passed launcher key initialization, MiniNDN startup, both Provider readiness/
offer decisions, and both `NDNSF_DI_GRANT_VERIFICATION` checks at
`BEFORE_ASSEMBLY`, then the unchanged host guard stopped at
`RESOURCE_BOUNDARY:MemAvailable` before requester ACK/Selection. Across 298
samples, available memory reached a minimum of `1003737088` bytes, owned swap
reached `524922880` bytes, swap-I/O delta reached `1107390464` bytes, and
aggregate RSS reached `9811668992` bytes. The assembly worker child reached
approximately `5112135680` bytes RSS while its provider parent was
approximately `2050000000` bytes RSS. Cleanup passed. The requester recorded
`CANCELLED`; no ACK_CLOSED/Selection, runner, execution, terminal, repeat
round, or oracle result exists. No checkbox changes; T005/T006/T007/T009
remain `PARTIAL`. Raw output is `.codex-tmp/spec189-r121-launch.log`, raw run
root is `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r121/`,
and durable detail is in [r121 resource evidence](evidence/b189-r119-range-source-focused-check-20260920.md).
The next step is a bounded C++ worker ownership repair: release parent-side
model/initializer buffers only after the request pipe is fully written, then
rebuild, rerun focused selectors, reinstall, and use a fresh guarded run root.

The r122 repair adds an optional `sourceToReleaseAfterWrite` argument to the
OA02 worker transport. Existing four-argument callers remain non-destructive;
production `NativeCanonicalOnnxAssembler` opts in and releases the parent
model/initializer/material source only after the complete request frame enters
the pipe. The affected targeted build completed `538/538`; the material
consumer selector passed `2/2`, and the combined ONNX extraction, native
assembly, activation, and Repo publication selector passed all `30` cases.
The installed five-target candidate has exact build/installed SHA-256 matches;
the corrected `ldd`/RPATH check found no unresolved or build-tree dependency.
No checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Raw outputs are
`.codex-tmp/spec189-r122-targeted-build.log`,
`.codex-tmp/spec189-r122-material-selector.log`,
`.codex-tmp/spec189-r122-onnx-repo-selector.log`, and
`.codex-tmp/spec189-r122-runtime-identity-v2.log`. The next step is the new
guarded MiniNDN run root `two-provider-global-r122`.

The fresh r122 MiniNDN run entered with the repaired installed candidate and
both Providers reached `NDNSF_DI_NATIVE_PROVIDER_READY`. Before requester
`ACK_DECISION` or Selection, the unchanged host guard stopped at
`RESOURCE_BOUNDARY:diskFree`: across 45 samples disk free reached a minimum of
`4001157120` bytes against `4294967296`; available memory stayed at or above
`6973997056` bytes, aggregate RSS peaked at `3527479296` bytes, owned swap at
`4096` bytes, and swap-I/O delta at `180342784` bytes. Cleanup passed and no
process remained. No requester ACK/Selection, grant verification, assembly,
runner, execution, terminal, repeat round, or oracle result exists. No
checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Raw output is
`.codex-tmp/spec189-r122-launch.log`, raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r122/`,
and durable detail is in [r122 disk evidence](evidence/b189-r119-range-source-focused-check-20260920.md).
The next step is a bounded disk-artifact/working-set review and a
space-preserving run setup; do not raise the guard or delete preserved failure
evidence implicitly.

The disk review found the root filesystem at 98% use and the old r118/r121
Repo payload copies were byte-identical to the canonical initializer. Their
existing paths were retained and hard-linked to the canonical immutable inode,
releasing approximately `2.2G`; the r122 incomplete staging `.part` digest did
not match and was left untouched. Root free space then measured
`7007141888` bytes. No code, installed candidate, guard limit, or durable raw
run was deleted. No checkbox changes; T005/T006/T007/T009 remain `PARTIAL`.
The next step is a fresh r123 guarded MiniNDN run using the same verified
candidate and a new run root, after recording this space-preserving boundary.

The first r123 invocation stopped before MiniNDN because the oracle digest
argument omitted a `4`; preflight returned
`SPEC189_ORACLE_BINARY_DIGEST_MISMATCH`. The verified build/installed digest
is `sha256:ef12b58e7fb2e3b0f0643ab7afafe02a3786456150c178a00402a3adef7b9086`.
No checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Raw output is
`.codex-tmp/spec189-r123-launch.log`. The next step is a fresh invocation
with only this digest corrected.

The corrected r124 invocation entered MiniNDN with the same installed
candidate, but before requester `ACK_DECISION`/Selection the unchanged guard
stopped at `RESOURCE_BOUNDARY:diskFree`; cleanup passed and no process
remained. The r124 run root accumulated approximately `4.0G`, dominated by
the requester encrypted Repo payload set; available memory stayed healthy.
No grant verification, assembly, layer fetch, runner, execution, terminal,
repeat round, or oracle result exists. No checkbox changes;
T005/T006/T007/T009 remain `PARTIAL`. Raw output is
`.codex-tmp/spec189-r124-launch.log`, raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r124/`,
and durable detail is in [r124 disk evidence](evidence/b189-r119-range-source-focused-check-20260920.md).
The next step is the bounded experiment-runner storage-path repair; it must
keep the guard and production ownership contract unchanged.

The r125 runner repair adds optional `--encrypted-repository-path` placement;
the default remains under the run root, while an explicit path must be a new
or empty directory outside that root. Focused Python regression and guard
tests passed `44` cases, and `py_compile` passed. No checkbox changes;
T005/T006/T007/T009 remain `PARTIAL`. Raw output is
`.codex-tmp/spec189-r125-runner-path-tests.log`. The next step is a fresh
guarded MiniNDN run with the external ciphertext path and the same installed
native candidate.

The fresh r125 run used `/dev/shm/ndnsf-spec189-r125/encrypted-repo` and
therefore passed the prior root-disk boundary: both Providers reached READY,
signed `ACK_DECISION`, and `NDNSF_DI_GRANT_VERIFICATION` at
`BEFORE_ASSEMBLY`; Provider-0 also entered assembly staging and fetched the
canonical ONNX. The unchanged host guard then stopped at
`RESOURCE_BOUNDARY:ownedSwap` after 284 samples: minimum available memory was
`2282303488`, minimum root disk free was `5001625600`, aggregate RSS peaked at
`6933381120`, and owned swap peaked at `302174208` against the unchanged
`268435456` limit. Cleanup passed and no process remained. There is no
`RUNNER_READY`, execution, terminal response, second request, or oracle result;
T005/T006/T007/T009 remain `PARTIAL`. Raw output is
`.codex-tmp/spec189-r125-launch.log`, raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r125/`,
and durable detail is in [r125 resource evidence](evidence/b189-r119-range-source-focused-check-20260920.md).
The tmpfs ciphertext placement is not a qualification result; after preserving
the run, all 24 validated canonical payload paths were hard-linked to the
immutable source, increasing ext4 free space to `20034101248` bytes. The next
step is a fresh r126 run using ext4-backed run-scoped ciphertext and the same
guard/candidate.

The first r130 invocation stopped before MiniNDN at
`ASSEMBLY_WORKER_BINARY_DIGEST_MISMATCH` because the manually supplied worker
hash had a character transposition. No checkbox changes and no protocol or
resource result exists. The verified build/installed worker digest is
`sha256:1a97d902e0c9a2e5d8acbaf34046a9a92ae6a77d9d494a27671b2deb75b3f4b6`.
Raw output is `.codex-tmp/spec189-r130-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r130/`.
The next step is r131 with hashes read directly from the verified files.

r126 used the ext4-backed external ciphertext directory and crossed the r125
tmpfs/swap boundary. Both Providers reached READY, signed `ACK_DECISION`, and
`NDNSF_DI_GRANT_VERIFICATION` at `BEFORE_ASSEMBLY`; Provider-0 fetched
`canonical.onnx` into assembly staging. The unchanged host guard then stopped
at `RESOURCE_BOUNDARY:MemAvailable` before `RUNNER_READY`, execution, or
terminal response. Across 299 samples, available memory reached a minimum of
`1174708224`, disk free a minimum of `16242237440`, aggregate RSS a maximum of
`8260157440`, owned swap a maximum of `105701376`, and swap-I/O delta a maximum
of `1005477888`. Cleanup passed and no process remained. No checkbox changes;
T005/T006/T007/T009 remain `PARTIAL`. Raw output is
`.codex-tmp/spec189-r126-launch.log`, raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r126/`,
and durable detail is in [r126 resource evidence](evidence/b189-r119-range-source-focused-check-20260920.md).
The next bounded change is a finite MiniNDN NFD Content Store size; guard
limits, native ownership, and production Repo/DI contracts remain unchanged.

The MiniNDN-only repair sets `MININDN_NFD_CS_SIZE=4096` instead of the
MiniNDN default 65536 entries, so forwarders do not each retain multi-GiB
canonical/large-data working sets. The authenticated Repo remains the source
of truth and the production NFD default, guard, and native ownership contracts
are unchanged. The focused Python and guard regression set passed `44` cases;
runner `py_compile` and `git diff --check` passed. No checkbox changes;
T005/T006/T007/T009 remain `PARTIAL`. The next step is a fresh guarded r127
MiniNDN run with ext4-backed encrypted Repo storage and the same installed
candidate.

r127 使用 `MININDN_NFD_CS_SIZE=4096` 后仍在
`RESOURCE_BOUNDARY:MemAvailable` 停止。两 Provider 到达 READY、ACK offer
和 `GRANT_VERIFICATION=BEFORE_ASSEMBLY`，但 worker RSS 峰值约
`6283599872`，aggregate RSS 峰值 `8255197184`；available memory 最低
`1493688320`，owned swap 最大 `238215168`，cleanup 通过。低 CS 降低了
NFD 常驻，却放大了 fetch worker 工作集，因此不计资源/协议 PASS；没有
`RUNNER_READY`、execution、terminal、第二请求或 oracle。T005/T006/T007/T009
仍为 `PARTIAL`。Raw output 为 `.codex-tmp/spec189-r127-launch.log`，raw
run root 为 `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r127/`。
下一步是保留 guard 不变，测试中间 NFD CS 容量。

The next bounded candidate sets `MININDN_NFD_CS_SIZE=32768`; this is a
MiniNDN-only cache parameter change and does not alter production NFD defaults,
Repo/DI ownership, or host guard limits. The required focused runner tests and
`py_compile` gate must pass before the fresh r128 run.

The first r128 invocation stopped in preflight at
`MODEL_TOKENIZER_DIGEST_MISMATCH` before MiniNDN; a direct hash check confirms
the stage-manifest tokenizer is
`sha256:aeb13307a71acd8fe81861d94ad54ab689df773318809eed3cbe794b4492dae4`.
This is an invocation boundary with no task checkbox change and no protocol or
resource result. Raw output is `.codex-tmp/spec189-r128-launch.log`; the fresh
r128 run root is `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r128/`.
The next step is r129 with the exact verified tokenizer digest.

r129 passed tokenizer preflight and used ext4-backed ciphertext with
`MININDN_NFD_CS_SIZE=32768`. Both Providers reached READY, ACK offer, and
`GRANT_VERIFICATION=BEFORE_ASSEMBLY`; Provider-0 fetched canonical ONNX, then
the unchanged guard stopped at `RESOURCE_BOUNDARY:MemAvailable`. Across 298
samples, available memory reached `1170264064`, disk free `8675835904`,
aggregate RSS `8783286272`, owned swap `105500672`, and swap-I/O delta
`740933632`; cleanup passed. The assembly worker peaked at about
`5636308992` RSS, so this is a native worker working-set boundary, not a
protocol or qualification result. No `RUNNER_READY`, execution, terminal,
second request, or oracle exists; T005/T006/T007/T009 remain `PARTIAL`.
Raw output is `.codex-tmp/spec189-r129-launch.log`, raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r129/`.
The next bounded change is the C++ certified-chain move/ownership repair; no
guard or NFD production default change is authorized by this evidence.

The r130 C++ repair moves the authenticated original protobuf into S5 shape
inference and retains only compact certified node bytes for S6 comparison;
the certified checks and protocol contracts are unchanged. Targeted build
`538/538`, material selector `2/2`, and ONNX/Repo selector `30` cases passed.
Requester, Provider, worker, oracle, and DI library build/installed hashes
match, and the installed closure has no unresolved or build-tree dependency.
No checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Raw outputs are
`.codex-tmp/spec189-r130-targeted-build-move-original.log`,
`.codex-tmp/spec189-r130-material-selector-move-original.log`,
`.codex-tmp/spec189-r130-onnx-repo-selector-move-original.log`, and
`.codex-tmp/spec189-r130-runtime-identity.log`. The next step is a fresh
guarded MiniNDN run using the repaired installed candidate.

The r131 fresh installed-runtime run used exact hashes derived from the
verified runtime files and the r130 certified-chain move repair. It passed
MiniNDN startup, both Provider READY/ACK offers, and both
`GRANT_VERIFICATION=BEFORE_ASSEMBLY` checks, but the unchanged host guard
stopped at `RESOURCE_BOUNDARY:ownedSwap` before requester ACK/Selection. The
303-sample stream recorded minimum available memory `1645875200`, minimum disk
free `4884680704`, maximum aggregate RSS `8443310080`, maximum owned swap
`405467136`, and maximum swap-I/O delta `968613888`; cleanup passed. The
requester only recorded `CANCELLED`; there is no Selection, runner, execution,
terminal, repeat request, or oracle result. No checkbox changes;
T005/T006/T007/T009 remain `PARTIAL`. Raw output is
`.codex-tmp/spec189-r131-launch.log`, raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r131/`.
The next bounded C++ change releases and scrubs the worker's consumed source
buffers immediately after `ownedSourceModel()` returns, before S4-S7 continue.

The first r132 targeted build stopped in compilation because the new release
point called `scrub()` on `NativeCanonicalByteBuffer`, while that helper exists
only on `MaterialPayload`. No binary or selector ran; the raw compiler output
is `.codex-tmp/spec189-r132-targeted-build-source-release.log`. The bounded
repair uses `OPENSSL_cleanse` on `initializerBytes->asVector()` before reset;
the next step is the same targeted build retry. Task checkboxes remain
unchanged.

The first r132 focused-selector command then used the non-existent filter
`Spec189CanonicalMaterialConsumer/*` and stopped at Boost.Test setup with
"no test cases matching filter"; the second command entered all `30` named
ONNX/Repo cases and passed. This is a selector invocation boundary, not a
source assertion result. Raw outputs are
`.codex-tmp/spec189-r132-material-selector-source-release.log` and
`.codex-tmp/spec189-r132-onnx-repo-selector-source-release.log`; enumerate the
actual built test tree and rerun the material consumer with the exact filter
before installation.

The exact r132 material selector then entered both cases but failed
`ExternalInitializerUsesBoundedChunksAndReassemblesAfterSelection`: payload
cleanup zeroed a shared initializer backing that was still owned by the source,
so the post-Selection round-trip comparison saw different bytes. The ONNX/Repo
selector remained `30`-case PASS. This is a C++ ownership assertion boundary,
not a MiniNDN result. The bounded repair makes `MaterialPayload::scrub()` a
no-op for shared `backing`/range views and clears only payload-owned bytes;
rebuild the affected targets and rerun the exact material selector.

r132c passed the affected build `538/538`, the exact material selector `2/2`,
and the ONNX extraction/assembly/activation/Repo selector `30/30`. The
source-release repair is therefore focused-tested, but it does not close
T003/T005/T006/T007/T009 or establish a protocol result. Raw outputs are
`.codex-tmp/spec189-r132c-targeted-build-shared-scrub.log`,
`.codex-tmp/spec189-r132c-material-selector-shared-scrub.log`, and
`.codex-tmp/spec189-r132c-onnx-repo-selector-shared-scrub.log`. The next step
is exact installation of the affected DI/Provider/worker/requester/oracle
candidate and loaded-path identity verification before a fresh guarded run.

r132c installed the affected DI library, Provider, assembly worker, requester,
and C++ oracle successfully. Each build/install pair has exact SHA-256
equality; the verified installed `ldd` closure contains no unresolved,
build-tree, or `.codex-tmp` dependency. Evidence is in
`.codex-tmp/spec189-r132c-runtime-identity-v3.log` and
`.codex-tmp/spec189-r132c-ldd-closure.log`. This is an installed-runtime
identity gate only; T005/T006/T007/T009 remain `PARTIAL`. The next step is the
fresh guarded MiniNDN run root `two-provider-global-r132`.

r132 entered MiniNDN and both Providers reached READY, but the unchanged host
guard stopped at `RESOURCE_BOUNDARY:diskFree` before requester ACK/Selection.
The 44-sample stream recorded minimum available memory `6371221504`, minimum
disk free `3850231808`, maximum aggregate RSS `3521368064`, owned swap `0`,
and swap-I/O delta `117432320`; cleanup passed. There is no grant
verification, Selection, assembly, runner, execution, terminal, repeat
request, or oracle result. Raw output is `.codex-tmp/spec189-r132-launch.log`,
raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r132/`.
The candidate is unchanged; the next step is a space-preserving review of
duplicate immutable canonical artifacts followed by a fresh r133 run root.

The duplicate `canonical-repo-initializer.bin` and
`canonical-initializer.bin` were independently SHA-256 verified as
`413814d6166b5958623e45c085c502f56498e45769c93de45b8c16c2f61c62dd`, then
retained under both original paths with shared inode `4377094` and mode `600`.
No raw run, source, installed binary, or guard setting was removed; root free
space increased by `1503268864` bytes. This artifact operation does not alter
the protocol/resource verdict. The next fresh guarded candidate is
`two-provider-global-r133`.

The first r133 invocation stopped in preflight with
`MODEL_CANONICAL_INITIALIZER_SOURCE_NOT_IMMUTABLE`: the space-preserving
hard-link operation had applied mode `600` to the shared inode, while the
launcher requires the canonical initializer source to have no write bits. No
MiniNDN process or request chain started. Raw output is
`.codex-tmp/spec189-r133-launch.log`; the bounded repair restores read-only
mode on the shared immutable inode before retry.

r134 restored mode `444` and entered MiniNDN with a fresh run root. Both
Providers reached READY, but the unchanged host guard stopped at
`RESOURCE_BOUNDARY:diskFree` before requester ACK/Selection. The 39-sample
stream recorded minimum available memory `6469636096`, minimum disk free
`3847790592`, aggregate RSS peak `3527966720`, owned-swap peak `53248`, and
swap-I/O delta `54505472`; cleanup passed. There is no grant verification,
ACK/Selection, assembly, runner, execution, terminal, repeat request, or
oracle result. Raw output is `.codex-tmp/spec189-r134-launch.log`; raw run
root is `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r134/`.
This is a host artifact-placement boundary, not a protocol or qualification
result. Before r135, the preserved large staging artifacts must be checked
for exact digest identity and space-preserving deduplicated without deleting
the raw run evidence.

The r134 staging `.part` was independently verified as the canonical
initializer (`sha256:413814d6166b5958623e45c085c502f56498e45769c93de45b8c16c2f61c62dd`,
1,503,264,768 bytes) and atomically replaced by a hard link to inode `4377094`;
the original r134 path and bytes remain, now mode `444`. The same exact-match
operation was applied only to the verified r126/r127/r129 payload paths; the
r91 and r132 staging files had different digests and were preserved unchanged.
The canonical inode now has `84` links and host free space is
`9853657088` bytes. This is space-preserving evidence maintenance, not a
runtime or qualification result. The next step is a fresh r135 guarded run;
the new Repo staging allocation must still be observed as a resource gate.

r135 crossed the staging disk gate and ran the installed native candidate.
Both Providers reached READY, emitted signed `ACK_DECISION` offers, and
recorded `GRANT_VERIFICATION` at `BEFORE_ASSEMBLY`. The first native round
then failed at the requester stream boundary:
`NATIVE_STREAM_FAILED boundary=stream`, with Core message
`stream event gap exceeded retry budget`. The 348-sample resource record
ended in `drained`, with minimum available memory `2122010624`, minimum disk
free `4563804160`, RSS peak `8165933056`, owned-swap peak `117878784`, and
swap-I/O delta `117878784`. Provider-0 left an assembly cache cipher and ORT
profile, but no authenticated Selection, complete assembly/runner, execution,
terminal response, repeat request, or C++ oracle result is established.
Raw output is `.codex-tmp/spec189-r135-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r135/`.
This is a real production stream/assembly boundary, not a qualification
result. T005/T006/T007/T009 remain `PARTIAL`; next is a trace-enabled fresh
run to distinguish Selection-status/progress delivery from Provider assembly
stall before changing any timeout or guard limit.

Before the trace retry, r135's preserved Repo payload was independently
verified as the same canonical initializer digest and atomically hard-linked
to inode `4377094`; its original path and bytes remain, now mode `444`. The
shared inode has `86` links and host free space is `7570694144` bytes. This is
space-preserving evidence maintenance only; the r135 stream boundary and all
task statuses are unchanged.

r136 used the trace-enabled command and again started both Providers, but the
unchanged host guard stopped the fresh run at `RESOURCE_BOUNDARY:diskFree`.
The 283-sample record ended in `drained`: minimum available memory
`5972131840`, minimum disk free `3786682368`, RSS peak `4455481344`,
owned-swap peak `103866368`, and swap-I/O delta `103866368`; cleanup passed.
The requester trace recorded `43` stream-retry expressions and `42` timeout
callbacks for the selected Provider-1 event prefix, while Provider logs had no
Selection-status/progress trace and only the pre-assembly grant boundary.
Provider-0 retained a `752097499`-byte staging `canonical.onnx` plus root
metadata. No Selection/assembly completion, terminal response, repeat request,
or oracle result is established. Raw output is `.codex-tmp/spec189-r136-launch.log`;
raw run root is `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r136/`.
This is a host disk boundary that interrupted the trace experiment, not a
protocol result. Before r137, verify/deduplicate only exact immutable payload
matches, then use a fresh trace run with lifecycle tracing; do not alter guard
or stream limits.

The preserved r136 Repo payload was independently SHA-256 verified as the
canonical initializer and atomically hard-linked to inode `4377094`; its
original path and bytes remain, now mode `444`. The shared inode has `88` links
and host free space is `5289746432` bytes. The non-matching-sized r136
`canonical.onnx` staging artifact remains untouched. This is
space-preserving evidence maintenance only; r137 still requires fresh-run
trace and resource evidence.

Before r137, the nine preserved `752097499`-byte `canonical.onnx` staging
paths were independently verified against digest
`sha256:258f367628b69a66eccf0973386b2eb80e2fce74cd64c2172d4f00a6e06abb94`.
The r112 path was retained as inode `4853426`; the other eight historical
paths, including r136, were atomically hard-linked to that inode. All paths
and bytes remain available, the anchor now has `9` links, and root ext4 free
space is `11306508288` bytes. No open deleted file was found. This is
space-preserving evidence maintenance only; r137 still requires fresh-run
trace and resource evidence and no task status changes.

**B189-3 r137 native stream-gap boundary**: 2026-09-20 — r137 used a fresh
run root and the unchanged installed candidate and limits. The parent command
set `NDNSF_TIMELINE_TRACE=1`, but `env_for()` did not forward timeline-trace
variables to child processes; the launcher was repaired to forward timeline,
sample-rate, and stream-packet timeline controls, and the Python syntax check
passed. The run passed MiniNDN startup, both Provider READY/ACK decisions, and
both `GRANT_VERIFICATION boundary=BEFORE_ASSEMBLY` checks. Provider-0 reached
native assembly and retained a protected `752308868`-byte model cipher and a
`961277`-byte ORT profile; Provider-1 produced no Selection/assembly artifact.
The requester recorded `89` stream-retry lines and ended with
`NATIVE_STREAM_FAILED: stream event gap exceeded retry budget`. The 347-sample
stream ended `drained`: minimum available memory `2480660480`, minimum disk
free `6009753600`, RSS peak `8092606464`, owned-swap peak `197283840`, and
swap-I/O delta `590598144`; cleanup passed and the host guard did not stop the
run. No Selection, complete runner, execution, terminal, repeat request, or
C++ oracle result is established. Raw output is
`.codex-tmp/spec189-r137-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r137/`.
T005/T006/T007/T009 remain `PARTIAL`; the next changed gate is a fresh run
with the repaired child-process trace propagation, without changing limits.

**B189-3 r138 trace-propagation disk boundary**: 2026-09-20 — r138 was a
fresh run after the launcher repair and verified that timeline, sample-rate,
and stream-packet trace variables reached both Provider environments. It
passed MiniNDN startup, both Provider READY/ACK decisions, and both
`GRANT_VERIFICATION boundary=BEFORE_ASSEMBLY` checks. Provider-0 fetched a
`752097499`-byte staging `canonical.onnx`; no Selection, assembly completion,
runner, execution, terminal, repeat request, or C++ oracle result was
observed. The unchanged host guard stopped at `RESOURCE_BOUNDARY:diskFree`;
287 samples recorded minimum available memory `5828329472`, minimum disk free
`3729842176`, RSS peak `4453687296`, owned-swap peak `103759872`, and swap-I/O
delta `296140800`. Cleanup passed and the requester recorded cancellation at
the request boundary. Raw output is `.codex-tmp/spec189-r138-launch.log`; raw
run root is `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r138/`.
T005/T006/T007/T009 remain `PARTIAL`; next is exact space-preserving
deduplication of verified initializer/staging payloads, then a fresh run with
the same limits.

After r138, both r137/r138 canonical Repo initializer payload paths were
independently verified as digest
`sha256:413814d6166b5958623e45c085c502f56498e45769c93de45b8c16c2f61c62dd`
and retained on initializer inode `4377094`, now with `93` links. The r138
`canonical.onnx` staging path was independently verified as digest
`sha256:258f367628b69a66eccf0973386b2eb80e2fce74cd64c2172d4f00a6e06abb94`
and retained on inode `4853426`, now with `10` links. All paths and bytes
remain available; root ext4 free space is `7488299008` bytes. This is
space-preserving evidence maintenance only; no task status changes.

A further exact SHA-256 check found the r131 canonical Repo initializer
payload at the same digest; it was atomically hard-linked to initializer inode
`4377094`. The original r131 path and bytes remain, the inode now has `94`
links, and root ext4 free space is `8991244288` bytes. The r91/r122/r132
`.part` files had different digests and were deliberately left untouched.
This is space-preserving evidence maintenance only; no task status changes.

**B189-3 r118 resource boundary**: 2026-09-20 — after all system binaries
were updated to the verified build, the fresh r118 run passed candidate
identity, MiniNDN startup, both Provider readiness/offer decisions, and both
`NDNSF_DI_GRANT_VERIFICATION` checks at `BEFORE_ASSEMBLY`. The unchanged host
guard then stopped at `RESOURCE_BOUNDARY:MemAvailable`: minimum available
memory was `1080655872` bytes against `1610612736`, owned swap reached
`329965568` against `268435456`, swap-I/O delta reached `1314488320` against
`268435456`, and aggregate sampled RSS reached `9308581888` bytes. Cleanup
passed and no process remained. The requester only recorded cancellation;
there is no ACK_CLOSED/Selection, ASSEMBLY_STARTED, runner, execution,
terminal, repeat round, or oracle result. Raw output is
`.codex-tmp/spec189-r118-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r118/`.
Durable detail is in [r118 resource evidence](evidence/b189-r118-resource-boundary-20260920.md).
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Do not raise
the guard limits or count this as protocol PASS; next step is read-only native
preparation/assembly memory attribution and a bounded C++ ownership repair.

**B189-3 r117 digest invocation boundary**: 2026-09-20 — after installing
the current system binaries, the r117 command did not reach MiniNDN because
the manifest digest argument contained a typo (`...1b974...`) instead of the
verified `stage-manifest-qwen-v2.json` digest. Preflight returned
`MODEL_STAGE_MANIFEST_DIGEST_MISMATCH`; the preserved supervisor reports
`boundary: null`, `cleanup: PASS`, `returncode: 1`, and no remaining
processes. Raw output is `.codex-tmp/spec189-r117-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r117/`.
This is an invocation boundary, not a source, protocol, resource, or model
result. Durable detail is in [r117 digest evidence](evidence/b189-r117-digest-invocation-boundary-20260920.md).
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is
a fresh r118 run with the exact verified manifest digest.

**B189-3 r116 preparation-source boundary**: 2026-09-20 — the fresh r116
candidate passed identity checks, MiniNDN Controller/Authority startup, and
reached the requester, but stopped before ACK/Selection because native
preparation reported `PREPARATION_SOURCE_UNAVAILABLE` with
`fallback initializer does not match pinned digest or size`. Both Provider
logs are empty; no request-chain protocol stage, resource qualification,
runner, execution, terminal, repeat round, or oracle result exists. The
preserved supervisor reports `boundary: null`, `cleanup: PASS`,
`returncode: 1`, and no remaining processes. Raw output is
`.codex-tmp/spec189-r116-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r116/`.
The r116 requester config and published source manifest pin initializer
digest `sha256:413814d6166b5958623e45c085c502f56498e45769c93de45b8c16c2f61c62dd`,
and the preserved fallback file has the same digest; the changed gate is
installed requester/library provenance or the preparation fallback contract,
not a resource limit. Durable detail is in [r116 preparation evidence](evidence/b189-r116-preparation-source-boundary-20260920.md).
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is
to install the current requester/authority/oracle targets from the verified
build tree, reverify their identities, then retry with a fresh run root.

**B189-3 r115 manifest preflight boundary**: 2026-09-20 — the first r115
launch used the older `stage-manifest-qwen-r99.json`, which has no required
`eosTokenIds`; candidate preflight stopped with
`MODEL_EOS_TOKEN_IDS_REQUIRED` before MiniNDN startup. The preserved r115
supervisor reports `boundary: null`, `cleanup: PASS`, `returncode: 1`, and no
remaining processes. Raw output is `.codex-tmp/spec189-r115-launch.log`, and
the preserved run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r115/`.
This is a launch-argument boundary, not a protocol or model result. Durable
detail is in [r115 manifest evidence](evidence/b189-r115-manifest-preflight-boundary-20260920.md).
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is
a fresh run root with the prepared `stage-manifest-qwen-v2.json` and its
candidate digest.

**B189-3 r115 materialized-role graph repair checkpoint**: 2026-09-20 — the
post-selection compact role now carries an authenticated `materializedRole`
recipe flag. The complete canonical-source checker is skipped only for this
internal materialized-role path; shape inference, boundary extraction,
assembled-model checking, and ORT session creation remain required. The
affected build passed `434/434`; the material consumer selectors passed `2/2`;
the ONNX extraction/assembly/activation selectors passed `25/25`; all three
affected global targets were installed; and the refreshed native receipt
verified with SHA-256
`71cefd68e6bdf9ccdb58bb389e00f188b604a16b732e127239884a985052886b`.
Installed hashes are DI library
`c853a48832041fa7cdedd3c6f9033a97191b9861089ac42394da5a37cfd24461`, Provider
`fcc9d208b791d98c580dca03d5896395209f712d15893c1765615d4b4a3f9427`, and
Worker `548276efaa05acea21fe8ef8a5e0e056fafdf97e07958bc36e2049cffa8052fc`.
`ldd` resolves the installed Provider/Worker closure through `/usr/local` and
`/opt/onnxruntime` with no `not found`. Durable detail is in [r115 repair
evidence](evidence/b189-r115-materialized-role-graph-repair-20260920.md).
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is
the fresh installed-runtime r115 MiniNDN two-Provider run.

**B189-3 r114 native graph boundary**: 2026-09-20 — the fresh installed-runtime
run passed candidate identity, MiniNDN startup, ACK closure, Selection, both
Provider grant checks, and Provider-0 `ASSEMBLY_STARTED`. It crossed the r112
resource boundary: the unchanged guard did not stop it, maximum owned swap was
`161386496` bytes against the `268435456`-byte limit, maximum RSS was
`6426677248` bytes, cleanup passed, and no process remained. Provider-0 then
failed at `DI_NATIVE_ONNX_GRAPH` after the material-fetch phase; Provider-1's
exact-Data terminal and the requester's stream failure are downstream. No
runner, successful ONNX execution, terminal success, repeat round, or oracle
result exists. Durable detail is in [r114 evidence](evidence/b189-r114-native-graph-boundary-20260920.md),
with raw run root under
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r114/`.
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is
a read-only review of the exact native graph substage before any retry.

**B189-3 r113 material-backing release repair checkpoint**: 2026-09-20 —
after r112 exposed a host resource boundary, the native materializer now
scrubs/releases selected payload backing after every selected node and
initializer has been authenticated and copied into the protobuf model, before
final serialization. The conservative assembly budget is unchanged. The
affected build passed `434/434`; the material consumer selectors passed `2/2`;
and `Spec182OnnxActivation` passed `9/9` with the explicit worker fixture
directory. The affected global targets were installed and the refreshed native
receipt verified with SHA-256
`d548b4cb6356d6210ff79591b7f4f45a7d76f98231ac281c1f40550a549cdd4f`.
MiniNDN revalidation remains pending. Durable detail is in [r113 repair
evidence](evidence/b189-r113-material-backing-release-20260920.md). No task
checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is a fresh
r114 installed-runtime MiniNDN run.

**B189-3 r113 focused-selector invocation boundary**: 2026-09-20 — the
post-memory-repair native build passed `434/434`, but the first focused
selector invocation omitted `NDNSF_SPEC182_BIN_DIR=build-spec189-oracle`.
The selectors therefore stopped before their assertion bodies because the
worker binaries were not discoverable. This is an incomplete validation
command, not a source or MiniNDN result. Raw logs are
`.codex-tmp/spec189-r113-material-selector.log` and
`.codex-tmp/spec189-r113-onnx-activation.log`; durable detail is in
[r113 invocation evidence](evidence/b189-r113-selector-invocation-boundary-20260920.md).
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is
to build the named worker fixtures if required, set the fixture path, rerun
the selectors, then install the changed candidate.

**B189-3 r112 owned-swap resource boundary**: 2026-09-20 — the fresh
installed-runtime run passed candidate identity, MiniNDN startup, ACK,
Selection, and both Provider grant checks. Provider-0 reached
`ASSEMBLY_STARTED`; Provider-1 reached `DEPENDENCY_FETCH` and later emitted a
downstream failed `TERMINAL` while Provider-0 was still assembling. The
unchanged host guard then stopped the supervised request at
`RESOURCE_BOUNDARY:ownedSwap`: first over-limit sample
`ownedSwapBytes=271503360` against `268435456`, with RSS
`5762756608` bytes and `swapIoDeltaBytes=634826752`. Cleanup passed and the
final sample drained to zero owned swap. No runner, successful ONNX execution,
terminal response, second round, or oracle result exists. Durable detail is in
[r112 evidence](evidence/b189-r112-owned-swap-boundary-20260920.md), with raw
run root under
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r112/`.
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is
a read-only native assembly memory review before choosing a bounded fix or
candidate resource decision.

**B189-3 r111 digest-format preflight boundary**: 2026-09-20 — the fresh
candidate dispatch stopped before MiniNDN because the launcher requires
`sha256:<hex>` identities while the command supplied the stage-manifest
identity as bare hex, producing `MODEL_STAGE_MANIFEST_DIGEST_MISMATCH`. No
process, protocol, resource, model, or oracle result exists; the supervisor
cleaned up successfully and the final resource sample had zero owned swap.
Durable detail is in [r111 evidence](evidence/b189-r111-digest-format-boundary-20260920.md),
with raw run root under
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r111/`.
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is
a fresh r112 run root with the required `sha256:` prefixes on every digest.

**B189-3 r110 material-template repair checkpoint**: 2026-09-20 — the r109
shared-range decode defect was repaired: materialized template and node graph
parsing now uses `MaterialPayload::data()`/`byteSize()` rather than the legacy
`.bytes` member. The affected native build passed `434/434`; focused material
consumer selectors passed `2/2`; `Spec182OnnxActivation` passed `9/9`; all
affected global targets were installed; and the refreshed native receipt
verified with SHA-256
`a1128c3fe7001c3b996bfefa5246c9ae518866edf4385478865f0ab6ddc20ef0`.
No MiniNDN result or task checkbox changes; T005/T006/T007/T009 remain
`PARTIAL`. Durable detail is in [r110 evidence](evidence/b189-r110-material-template-repair-20260920.md).
Next step is a fresh installed-runtime MiniNDN run with a new run root.

**B189-3 r109 material-template boundary**: 2026-09-20 — the repaired
installed runtime passed candidate preflight, MiniNDN startup, ACK, Selection,
Provider grant verification, and `ASSEMBLY_STARTED`. Provider-0 verified 4,633
material payloads and 817 bundles, then failed at
`DI_NATIVE_ONNX_MATERIAL_TEMPLATE` because the new shared-range payload kept
its bytes in `MaterialPayload::backing` while the template decode still read
the legacy `.bytes` member. Provider-1's exact-Data dependency failure was
downstream. The resource guard did not stop the run; cleanup passed and the
final sample was drained with zero owned swap. Durable detail is in [r109
evidence](evidence/b189-r109-material-template-boundary-20260920.md), with
raw run root under `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r109/`.
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next changed
gate: use `data()`/`byteSize()` for every materialized template/node decode,
then rerun focused C++/installed checks and a fresh MiniNDN run.

**B189-3 r108 canonical-source preflight boundary**: 2026-09-20 — the new
changed-candidate launch stopped before MiniNDN because the command supplied
`candidate/canonical/canonical-qwen-external.onnx`, while the verified source
is in the campaign-level `canonical/` directory. The launcher reported
`MODEL_CANONICAL_SOURCE_MISSING`; no process, protocol, resource, model, or
oracle result exists. Raw output is `.codex-tmp/spec189-r108-launch.log`, with
durable detail in [r108 evidence](evidence/b189-r108-canonical-source-preflight-20260920.md).
This is a launch-argument boundary, not a product result. No task checkbox
changes; T005/T006/T007/T009 remain `PARTIAL`. Retry with a fresh run root and
the verified canonical source path.

**B189-3 r106 native material-budget boundary**: 2026-09-20 — the fresh
installed-runtime MiniNDN run passed candidate preflight, startup, ACK,
Selection, Provider grant verification, and native assembly admission on both
Providers. Provider-0 verified 5,453 material payloads totalling
`1511365303` bytes, then failed at the first native assembly boundary with
`DI_NATIVE_ONNX_MATERIAL_INITIALIZER`; the candidate's authenticated
`max_assembled_bytes=1571325451` is below the assembler's retained-material,
raw-initializer, and final-model working-set accounting. Provider-1's signed
dependency fetch failure followed Provider-0 termination. The resource guard
did not stop the run and cleanup passed (`returncode=1`, `boundary=null`, no
remaining processes). No runner, ONNX execution, terminal response,
second-round result, or oracle result exists. Durable detail is in [r106
evidence](evidence/b189-r106-native-material-budget-20260920.md), with raw
run root under `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r106/`.
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next changed
gate: reconcile the planner's per-role assembly budget with the native
assembler's actual bounded working-set contract, then rerun static review,
focused native checks, installed identity checks, and a fresh MiniNDN run.

**B189-3 r107 native material-budget repair checkpoint**: 2026-09-20 — the
r106 gate was repaired in the C++ consumer and Qwen profile: selected external
initializer chunks now retain authenticated shared bundle ranges instead of
duplicating every bundle, and the scrubber clears shared backing allocations;
the profile now derives `max_assembled_bytes` from the assembler's conservative
working-set upper bound. The affected DI/integration and unit targets built,
the material consumer selectors passed 2/2, `Spec182OnnxActivation` passed
9/9, and affected global targets were installed with the canonical dependency
closure. The combined publisher selector still reaches the known staging-file
`Permission denied` environment boundary; it is not counted as a product
failure or PASS. Receipt verification passed with SHA-256
`6a3c8f5fa690f71161d4db389d0d23b2b54b3b769890a936b6a226b49baaa7f1`.
Durable detail is in [r107 evidence](evidence/b189-r107-native-material-budget-repair-20260920.md).
No MiniNDN result or task checkbox changes; T005/T006/T007/T009 remain
`PARTIAL`. Next step is a fresh changed-candidate installed-runtime run.

**B189-3 r105 installed-runtime preflight boundary**: 2026-09-20 — the
two-node stage count was corrected, but the fresh dispatch stopped before
MiniNDN at `MODEL_NODE_MAPPING_MISSING` because the command referenced
`candidate/qwen-node-mapping.json` instead of the existing canonical
`candidate/node-mapping.json`. No process, protocol, resource, or model result
exists. Raw output is `.codex-tmp/spec189-r105-launch.log`; use the existing
mapping only after verifying its digest and keep a new run root. No task
checkbox changes; T005/T006/T007/T009 remain `PARTIAL`.

**B189-3 r104 installed-runtime preflight boundary**: 2026-09-20 — the
first fresh installed-runtime dispatch stopped before MiniNDN because the
launcher defaulted to three `--stage-nodes` while the authenticated Qwen v2
manifest contains two stages: `--stage-nodes count must match stage manifest
and contain no duplicates`. No process, protocol, resource, or model result
exists. Raw output is `.codex-tmp/spec189-r104-launch.log`; retry with the
explicit two-node topology input and the same candidate/binary/resource
identities. No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`.

**B189-3 r103 assembly-timeout wiring checkpoint**: 2026-09-20 — source
review identified and repaired the standalone native Provider's missing
`assemblyTimeoutMs` assignment. The executable now exposes and logs a finite
independent assembly budget; the Qwen profile passes 900,000 ms while keeping
dependency-fetch/readiness/control budgets separate. The launcher now accepts
explicit installed binary paths, and `di-native-provider` has a canonical
BINDIR install path. Affected C++ build passed (`302/302`), the complete
`Spec182OnnxActivation` selector passed 9/9 after building its five worker
fixtures, and the affected Provider/worker/Authority/Requester/oracle targets
were installed with `/usr/local`/`/opt/onnxruntime` dependency closure checks.
See [r103 evidence](evidence/b189-r103-assembly-timeout-wiring-20260920.md).
The maintained build receipt was refreshed and independently verified
(`55a44f07b4760b6f607143e7797cff24effc202359f85d004031aec8404bf26c`). The
Controller was also installed so every MiniNDN executable can be supplied from
the canonical global runtime. A new
installed-runtime MiniNDN run with fresh run identity and unchanged
model/resource inputs is now the next gate. No task checkbox changes;
T005/T006/T007/T009 remain `PARTIAL`.

**B189-3 r103 worker-fixture build boundary**: 2026-09-20 — the retry build
for the missing `spec182-worker-tool-*` fixtures stopped before compilation
because the existing build-tree subdirectory
`build-spec189-oracle/tests/standalone/spec182-worker-tools` is owned by
`root:root` from an earlier root experiment. This is a build-environment
permission boundary, not a source or product result. Raw output is
`.codex-tmp/spec189-r103-worker-fixtures-build.log`; repair only that explicit
build subdirectory ownership, then rerun the same named targets. No task
checkbox changes; T005/T006/T007/T009 remain `PARTIAL`.

**B189-3 r103 focused-selector invocation boundary**: 2026-09-20 — the
affected `di-native-provider` and `unit-tests` targets compiled and linked
successfully after wiring the explicit assembly budget. The first
`Spec182OnnxActivation` selector invocation passed 7/9 cases but stopped in
the two cases that require the separately registered
`spec182-worker-tool-block` and `spec182-worker-tool-sigkill` fixtures; the
invocation had not built those targets. This is an incomplete validation
command, not a product result. The raw output is
`.codex-tmp/spec189-r103-onnx-activation.log`; build the named worker targets
and rerun the selector before installing or launching a new MiniNDN candidate.
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`.

**B189-3 r102 native assembly-timeout checkpoint**: 2026-09-20 — the fresh
diagnostic run passed candidate/installed-runtime preflight and crossed the
r101 stream-only boundary: requester Selection commit and both Provider
Selection acceptance, `GRANT_VERIFIED`, and `ASSEMBLY_ADMISSION_REPORTED` were
observed. Provider-0 entered `ASSEMBLY_STARTED` and verified material fetches,
then failed with `DI_NATIVE_ONNX_ASSEMBLY_TIMEOUT`; Provider-1 failed its
dependent signed exact-Data fetch. The requester stream-gap error is recorded
as a downstream symptom. The unchanged resource guard did not stop the run
(`maxOwnedSwapBytes=116293632`, `minAvailableBytes=5158629376`,
`minDiskFreeBytes=26541301760`); cleanup passed, but there was no successful
runner, execution, terminal response, two-round output, or oracle result.
T005/T006/T007/T009 remain `PARTIAL`. See [r102 evidence](evidence/b189-r102-native-assembly-timeout-20260920.md).
Source/timing diagnosis now identifies the first boundary: Provider-0 entered
assembly at `1789912424.051127`, verified the last material payload at
`1789912603.290546`, and failed at `1789912603.579023`. The native Provider
executable did not wire `NativeCanonicalOnnxAssemblerOptions.assemblyTimeoutMs`,
leaving its 30,000 ms default active while the assembler started that deadline
before synchronous material fetches. The worker therefore observed an expired
deadline after the fetch and reported `DI_NATIVE_ONNX_ASSEMBLY_TIMEOUT`.
Next changed gate: add an explicit finite assembly budget independent from
dependency-fetch/readiness/control budgets, log its effective value, then run
read-only static review, affected build/install/identity checks, and a new
run-scoped MiniNDN candidate. This does not close any task or qualify the
protocol; T005/T006/T007/T009 remain `PARTIAL`.

**B189-3 r101 native stream-gap checkpoint**: 2026-09-20 — the candidate
preflight and real MiniNDN startup passed; both Providers emitted
`ACK_DECISION` and `GRANT_VERIFIED` before assembly, and Repo publication
created the run-scoped encrypted manifests. The first C++ requester request
failed with `NATIVE_STREAM_FAILED` / `stream event gap exceeded retry budget`
at the `stream` boundary; supervisor cleanup passed, and the guard did not
stop for owned swap. No Selection, placement-bound fetch, assembly, runner,
terminal response, or oracle result was recorded. Diagnose the exact stream
event publication/retention/retry boundary before r102. See [r101 evidence](evidence/b189-r101-native-stream-gap-20260920.md).
T005/T006/T007/T009 remain `PARTIAL`.

The diagnostic-only launcher adjustment passed Python syntax and diff checks:
`env_for` now forwards the existing `SPEC175_TRACE` control to MiniNDN child
processes. It does not change retry budgets or acceptance semantics; r102 is
the first runtime using this observation control.

**B189-3 r101 install invocation boundary**: 2026-09-20 — the first
candidate-install invocation passed the shell script to Python and stopped with
`SyntaxError` before build/install or MiniNDN. The raw output is
`.codex-tmp/spec189-r101-install.log`; retry with the script interpreter. No
task is complete and T005/T006/T007/T009 remain `PARTIAL`.

**B189-3 r100 EOS-token preflight checkpoint**: 2026-09-20 — the repaired
Qwen stage-manifest copy passed family/schema/stage validation but the launcher
stopped before MiniNDN at `MODEL_EOS_TOKEN_IDS_REQUIRED`; the older manifest
does not carry the prepared stop-token contract. No task checkbox changes and
no protocol/resource result. Recover EOS IDs from the immutable tokenizer/model
metadata, validate a new manifest copy, and retry under a new run ID. T005/
T006/T007/T009 remain `PARTIAL`; see [r100 EOS evidence](evidence/b189-r100-eos-token-preflight-boundary-20260920.md).

**B189-3 r99 stage-manifest preflight checkpoint**: 2026-09-20 — the new
launcher invocation passed the refreshed candidate identity arguments but
stopped before MiniNDN at `ValueError: stage manifest requires an explicit
modelFamily`; the existing candidate stage manifest is from an older contract.
No task checkbox changes and no protocol/resource result. Recover and verify
the immutable Qwen stage manifest, then retry under a new run ID. T005/T006/
T007/T009 remain `PARTIAL`; see [r99 stage-manifest evidence](evidence/b189-r99-stage-manifest-family-boundary-20260920.md).

**B189-3 r99 native binding compile checkpoint**: 2026-09-20 — the maintained
native-build helper rebuilt the Waf/examples closure, then its forced Python
binding compile stopped at `di_bindings.cpp:688` because the new shared byte
buffer wrapper did not accept the old iterator-pair `optional.emplace` call.
No task checkbox changes and no MiniNDN process started. Replace that call
with explicit vector construction, refresh the helper receipt, and rerun
independent verify. See [r99 binding boundary evidence](evidence/b189-r99-native-build-binding-boundary-20260920.md).

**B189-3 r99 native identity stale-source checkpoint**: 2026-09-20 — the
zero-copy producer material-range change passed focused C++ selectors and the
target-scoped DI install completed, but the immediate maintained native
identity verification stopped before MiniNDN at
`SPEC180_NATIVE_IDENTITY_REJECTED: STALE_SOURCES` because the old build receipt
predated the source change. No task checkbox changes. Refresh the receipt with
the maintained native-build helper, rerun independent verify, and recompute
the full candidate digest map before a new MiniNDN run. T005/T006/T007/T009
remain `PARTIAL`; see [r99 stale-source evidence](evidence/b189-r99-native-verify-stale-sources-20260920.md).

**B189-3 r98 owned-swap resource checkpoint**: 2026-09-20 — the fresh root
r98 invocation satisfied the installed-candidate, full `sha256:` digest, PATH,
and resource preflight gates and started MiniNDN. Both Providers reached
`READY`, `ACK_DECISION`, and `GRANT_VERIFIED(BEFORE_ASSEMBLY)`, then the
maintained host guard stopped the request at
`RESOURCE_BOUNDARY:ownedSwap` because the run exceeded the unchanged 256 MiB
owned-swap limit. Cleanup passed; no Selection, selected-material fetch,
assembly, runner, handoff, terminal, token, or repeat result was observed.
T005/T006/T007/T009 remain `PARTIAL`; no checkbox changes. See [r98 evidence](evidence/b189-native-r98-owned-swap-boundary-20260920.md).
The next gate is a new resource-controlled run after inspecting/reducing the
working set within the production path, while retaining the complete identity
contract and maintained resource limits.

**B189-3 r97 candidate digest-format checkpoint**: 2026-09-20 — root r97
passed the host resource guard but stopped before MiniNDN because the command
passed bare hexadecimal digests while `require_file_digest()` requires the
`sha256:<hex>` form. No protocol stage started. The changed gate for r98 is a
complete prefixed digest map verified against the launcher helper; T006/T007/
T009 remain `PARTIAL`. See [r97 evidence](evidence/b189-native-r97-digest-format-boundary-20260920.md).

**B189-3 r96 launcher privilege checkpoint**: 2026-09-20 — after restoring
the declared swap contract, r96 passed the host resource guard but the
ordinary-user invocation stopped before MiniNDN at
`MININDN_REQUIRES_ROOT: run this script with sudo -E`. No protocol stage
started. The maintained root boundary is now the changed gate for r97; keep
all candidate digests and resource thresholds unchanged. T006/T007/T009 remain
`PARTIAL`; see [r96 evidence](evidence/b189-native-r96-launcher-root-boundary-20260920.md).

**B189-3 r95 resource-contract checkpoint**: 2026-09-20 — after r94's
`swapIo` boundary, the host swapfile was temporarily disabled to eliminate
swap I/O, but the maintained guard correctly rejected r95 at
`RESOURCE_BOUNDARY:SwapFree` because its unchanged contract requires 512 MiB
free swap. No MiniNDN process or protocol stage started. Restore the declared
swap capacity, verify free swap/available memory/disk and a stable `vmstat`
window, then retry as a new r96 run. T006/T007/T009 remain `PARTIAL`; see
[r95 evidence](evidence/b189-native-r95-resource-boundary-20260920.md).

**B189-3 r92-r94 launcher/resource checkpoints**: 2026-09-20 — r92 stopped
before MiniNDN because the launcher invocation omitted required installed
binary/build-receipt digests; r93 passed those identity checks but lacked
`/usr/local/bin` and stopped in MiniNDN cleanup at `nfd-stop`; r94 crossed the
PATH repair and stopped before topology startup at
`RESOURCE_BOUNDARY:swapIo`. All three attempts are preserved as
`UNQUALIFIED` launcher/host boundaries and do not advance T006/T007/T009.
The next changed gate is a sustained host resource preflight with new run ID,
zero new swap-in/out during the guard window, and materially more than the
four-GiB disk floor. See [r92](evidence/b189-native-r92-preflight-boundary-20260920.md),
[r93](evidence/b189-native-r93-preflight-boundary-20260920.md), and
[r94](evidence/b189-native-r94-resource-boundary-20260920.md).

**B189 target-bound lifecycle/r91 checkpoint**: 2026-09-20 — 官方只读
`review-agent` 对 Provider cache/runner lifetime、canonical artifact path 和
assembler final-directory cleanup 的冻结 r8 快照返回 `STATIC_PASS`。DI 目标闭包
以系统依赖和 Waf `-j4` 编译通过（556/556），Provider lifecycle、stream、prepare、
material consumer 和 production integration 的 C++ 定向 selector 均通过；维护的
target-scoped install helper 也完成了 `ndnsf-distributed-inference` 安装。一次无范围
的根 `waf install` 调度 2284 tasks 后中止，已登记为安装边界，不能当产品失败。
使用全新安装候选的真实 Qwen r91 已启动 MiniNDN 和两个 native Provider，并都达到
`NDNSF_DI_NATIVE_PROVIDER_READY`；随后宿主 `diskFree` 低于 4 GiB 守护阈值而停止，
cleanup PASS，但没有 ACK、Selection、assembly、runner、terminal 或 token。T006/T007/
T009 继续 `PARTIAL`，下一次必须在更大磁盘余量和新 run ID 下重试。完整静态、构建、
资源边界和五 lane 记录见 [r8/r91 evidence](evidence/b189-memory-lifecycle-r8-and-r91-20260920.md)。
API reference 已重新生成当前声明；`Design/test_design_state.py` 通过，但完整
`Design/verify-api-reference.py` 仍因混合工作树的源码快照漂移和旧 target reference
失败，故文档交付门仍为 `PARTIAL`。
盘点确认 r91 的首要宿主原因是重复 Qwen run staging、Git 临时 pack、CodeGraph
索引和多份旧安装候选；单个 base SIF 约 4.09 GB，不是主要增长来源。当前磁盘审计
及 deleted-but-open Codex 文件句柄记录在同一份 [r8/r91 evidence](evidence/b189-memory-lifecycle-r8-and-r91-20260920.md) 中。
随后按 allow-list 清理了未被证据引用的旧 Qwen runs、四个 superseded 安装候选和 Git
临时 pack；CodeGraph 已在忽略实验暂存后重建。清理明细见
[cleanup evidence](evidence/cleanup-20260920.json)，当前工作树仍保持 `IN_PROGRESS`，未
改变 Qwen 全链路验收状态。

**B189 shared-chain diagnosis checkpoint**: 2026-09-20 04:34 -0500 — 重新对照 YOLO 与 Qwen 真实运行边界：
两者共用 prepare/Repo/request/ACK/Selection/grant ingress，r47/r70 已证明 Qwen 能进入
ACK、Selection、授权验证和 selected-material fetch；因此当前阻断不是共同入口不兼容。
YOLO 的 `NATIVE_POSTPROCESS` 通过不覆盖 Qwen 的分阶段材料、hidden-state handoff、ORT
runner 和长时间 assembly。r30–r33 的 `RESOURCE_BOUNDARY:swapIo` 属于宿主资源门，另行
保留；r70 的首个生产缺陷是 terminal consumer 未接受 nonterminal Provider 的
authenticated progress，导致 assembly/dependency 等待期间 stream gap。当前 progress
binding 修复已有 C++ selector/静态证据，但尚未用新安装候选完成真实重跑；T006/T007/T009
保持 `PARTIAL`，不能用 YOLO、timeout 或静态 PASS 关闭。
本次 Spec/plan/tasks/batch/traceability 修订的五 lane 记录见
[boundary revision evidence](evidence/b189-spec-revision-20260920.md)。

**B189 memory/SmolLM2 checkpoint**: 2026-09-20 — C++
`ProviderArtifactCache` 生命周期修复已通过官方只读 `STATIC_PASS`，并以 `-j3`
完成受影响目标编译；stopped-active/replacement/cleanup 及 SmolLM2 catalog/planner
selector 均通过。修正后的 exporter 重新生成真实
`HuggingFaceTB/SmolLM2-135M` 两阶段 ONNX：326,168,079 与 326,171,617 bytes，
manifest 含 `modelFamily=llama`、EOS `[0]`、层范围和匹配 SHA-256，整模型/分阶段
top-token 均为 28。`plan_pipeline.py` 已按 family 选择默认模型并拒绝 Qwen/SmolLM2
错绑。stage 文件仅是 exporter-side 契约制品；生产 Provider 必须沿
`prepare → Repo → request(reference/input) → ACK → Selection → selected material
fetch/assembly/execute → terminal` 运行。当前冻结范围的官方复审为 `STATIC_PASS`；完整
两 Provider MiniNDN 尚未运行，任务保持 `PARTIAL`。见
[本批证据](evidence/b189-memory-smollm-batch-20260920.md)。

**B189 native memory-lifecycle audit**: 2026-09-20 — 静态检查发现 prepare publication
原来同时保留完整 canonical source/initializer 与 material manifest；post-Selection
assembly 在 role model 已物化后仍把 selected payloads/manifest 带入 worker。当前改动用
不可变 `shared_ptr` 原子替换释放 prepare 的 full source，并在
`materializeNativeCanonicalModel()` 后释放 Provider 侧 selected material。DI 与
`spec189-canonical-publisher` 编译成功，material-only publication、owning source
snapshot、queued cancellation 和 source-after-cancel rollback 定向 selector 均通过；
完整 `Spec182CanonicalPublisher` 在可写 run-scoped staging 下 14/14 通过，默认
root-owned `/tmp` 目录的权限失败保留为宿主配置边界。官方 `review-agent` 复审现已返回
`STATIC_PASS / TESTS_DEFERRED`；这只关闭本次内存
生命周期静态门，不关闭任何产品任务。
随后又收窄 `ModelPreparationCache` 的 owning source 作用域，确保 publication 前旧
full source 不被局部 `shared_ptr` 保持；增量重建 45.967s，两个定向 selector 再次通过。
完整覆盖、失败边界和下一步见
[memory lifecycle static audit](evidence/b189-memory-lifecycle-static-20260920.md)。

**B189 architecture-boundary correction**: 2026-09-20 — 复核确认 exporter 生成的
两阶段 ONNX 只用于输入/输出、EOS、层范围和 top-token 契约校验；Provider manifest
不再携带 exporter-local stage path，真实 C++ Provider 仍须在认证 Selection 后从
canonical Repo publication 获取被分配材料并组装 runner。与此同时，当前
`NativeQwenLayerSplit` 仍从 catalog 读取单个预声明 layer-range 候选，ACK 后决定
的是 Provider placement，而不是从多个切分候选中重新决定 range。文档已将此状态从
“ACK determines partition”修正为“ACK constrains placement; Selection authorizes
materialization”；真正的 ACK-driven partition 仍是未完成能力，不能由 stage 文件
或静态 placement 通过替代。产品和 MiniNDN 状态不变，仍为 `PARTIAL`。

**B189 exporter/reuse contract repair**: 2026-09-20 — 只读复审发现并修正维护 helper
的三个契约缺口：runtime manifest 的 `stages` 实际是整数而非 stage 列表；service/runtime
的 canonical revision 在 loader 解析 commit hash 后必须一致；EOS token 校验不能接受
Python `bool`。ONNX `SplitArtifact` 现在显式标记 `materialization=exporter-contract-only`，
提醒共享 Python policy 不能把本地 stage 文件当成 native Provider 输入。`py_compile` 与
限定 `git diff --check` 通过；随后按当前全局 C++/DI 安装重建 Python binding，
Spec175 ONNX boundary 与 Spec107 artifact reuse 定向测试共 `18 passed`。Python
provider 的 service-policy、eager preload、can-prepare 和 Selection preparation 路径均拒绝
`exporter-contract-only` stage；显式 local artifact 标为 `operator-local-unverified`，
没有 Repo registration 时拒绝。真实 MiniNDN 和产品状态不变。

**B189-3 host-native r70 progressed stream checkpoint**: 2026-09-19 — the
verified r65 candidate passed launch/resource gates, closed ACK and committed
Selection. Provider 1 reached `GRANT_VERIFIED`,
`ASSEMBLY_ADMISSION_REPORTED`, `EXECUTION_ENTERED` and `DEPENDENCY_FETCH`; Provider
0 emitted repeated authenticated selected-material `begin/returned/verified`
records. The requester still stopped at `NATIVE_STREAM_FAILED / stream event gap
exceeded retry budget` while assembly/dependency reads were active. Cleanup and
resource floors passed, but no runner, terminal response or token was observed.
The raw run and boundary diagnosis are preserved in [r70 evidence](evidence/b189-native-r70-progressed-stream-boundary-20260919.md).
This changes the next gate to a reviewed Core collaboration progress binding:
the terminal stream consumer must accept only exact `{provider,
role-specific assembly-progress operationId}` pairs from the committed
Selection, while preserving provider/member identity and monotonic freshness.
T003/T006/T007 remain `PARTIAL` and T009 remains blocked.

**B189-3 host-native r66 preflight checkpoint**: 2026-09-19 — the exact
candidate digests passed, but the root runner lacked the user-installed
`ndn.encoding` module while decoding Provider PIB keys and stopped with
`provider key prefix unavailable` before MiniNDN. The raw run is retained;
r67 supplies the explicit maintained MiniNDN Python path. T003/T006/T007
remain `PARTIAL` and T009 remains blocked. See [r66 preflight evidence](evidence/b189-native-r66-preflight-boundary-20260919.md).

**B189-3 host-native r65 preflight checkpoint**: 2026-09-19 — the reviewed
diagnostic candidate compiled and the 15-case C++ CollaborationStatus selector
passed, but the first r65 launcher command supplied a truncated node-mapping
digest and stopped at `MODEL_NODE_MAPPING_DIGEST_MISMATCH` before MiniNDN.
The raw supervisor record is retained; r66 must use the exact unchanged digest
and a new run directory. T003/T006/T007 remain `PARTIAL` and T009 remains
blocked. See [r65 preflight evidence](evidence/b189-native-r65-preflight-boundary-20260919.md).

**B189-3 host-native r64 checkpoint**: 2026-09-19 — the system-installed
candidate passed all preflight gates, MiniNDN startup, both Provider readiness,
ACK/Selection and grant verification. The requester then stopped at
`NATIVE_STREAM_FAILED / stream event gap exceeded retry budget`; no assembly,
runner, execution, terminal response or model token was observed. Resource and
cleanup gates passed. The changed gate is the post-grant admission/progress
handoff; T003/T006/T007 remain `PARTIAL` and T009 remains blocked. See [r64
stream-boundary evidence](evidence/b189-native-r64-stream-boundary-20260919.md).

**B189-3 host-native r63 preflight checkpoint**: 2026-09-19 — stage identity
and two-node topology checks passed, then a hand-typed controller digest missed
one character and stopped the launcher before MiniNDN. The installed binary and
manifest agree; no product status changed. See [r63 preflight evidence](evidence/b189-native-r63-preflight-boundary-20260919.md).

**B189-3 host-native r62 preflight checkpoint**: 2026-09-19 — the corrected
stage digest passed, then the launcher rejected the default three-node list
against the candidate's two stages. No process started and no product status
changed; r63 will pass exactly two stage nodes. See [r62 preflight evidence](evidence/b189-native-r62-preflight-boundary-20260919.md).

**B189-3 host-native r61 preflight checkpoint**: 2026-09-19 — the new run was
rejected before MiniNDN because the supplied stage-manifest digest was stale.
No process or protocol path started; the actual unchanged candidate digest was
recomputed and recorded. This launch-only failure does not change T003/T006/T007
or T009 status. See [r61 preflight evidence](evidence/b189-native-r61-preflight-boundary-20260919.md).

**B189-3 host-native r60 resource checkpoint**: 2026-09-19 — the system-installed
Spec189 runtime passed candidate/linker preflight, but the fresh
`two-provider-global-r60` attempt stopped at `RESOURCE_BOUNDARY:diskFree` before
ACK/Selection. Supervisor cleanup passed and the raw run is retained. The four
GiB disk guard is unchanged; the next retry must remove duplicate immutable
initializer storage or warm the Repo without deleting raw evidence. 43 canonical
initializer paths and 12 confirmed Repo payload paths were hard-linked; the
identity-mismatching r60 staging `.part` was retained. No provider assembly,
terminal result or qualification was observed. T003/T006/T007 remain `PARTIAL`;
T009 remains blocked. See [r60 disk-boundary evidence](evidence/b189-native-r60-disk-boundary-20260919.md).

**B189-3 host-native r59 checkpoint**: 2026-09-19 — removed 808 rebuildable
host build intermediates (4.3 GiB) while retaining raw r44–r59 evidence and
the six native binary hashes. The host-native retry passed the resource guard;
both Providers reached `READY`, ACK offers and `GRANT_VERIFICATION`, then the
requester stopped at `NATIVE_STREAM_FAILED / stream event gap exceeded retry
budget` before any assembly admission marker. Cleanup passed and no SIF,
Apptainer or Tiger runtime was involved. T006/T007 remain `PARTIAL`; T009
remains blocked. See [r59 evidence](evidence/b189-native-r59-20260919.md).

**B189-3 T006-R1 reporter-sequence checkpoint**: 2026-09-19 — the executable
runner factory now passes the Selection-owned assembly progress counter to its
post-admission reporter. The frozen repair passed read-only `STATIC_PASS`; the
first global-r3 compile exposed only missing `ndnsf::di::` test qualifiers,
which was retained in the raw build log and corrected under review. The rerun
compiled `unit-tests` and `di-native-provider` 302/302 with Waf `-j4`, and the
new reporter-contract selector plus all 15 `CollaborationStatus` cases passed.
This proves only the sequence contract; a fresh real MiniNDN run is still
required and T006/T007/T009 remain `PARTIAL`.

**B189-3 admission-sequence repair / r58 checkpoint**: 2026-09-19 — the
v4 frozen scope passed official read-only `STATIC_PASS`. A Selection-scoped
runtime sequence counter now survives Provider runner-factory copies and
rebuilds; local C++ lifecycle 1→2→3 and the complete assembly suite passed
9/9 after building the worker from the repository root. The fresh
`two-provider-global-r58` run stopped before ACK/Selection at
`RESOURCE_BOUNDARY:diskFree`; cleanup passed, memory stayed above 4.24 GiB,
and no protocol or model result was observed. T006/T007 remain `PARTIAL` and
T009 remains blocked by B189-3. See [admission sequence/r58 evidence](evidence/b189-admission-sequence-r58-20260919.md).

**B189-3 real Qwen r56 checkpoint**: 2026-09-19 — the fresh
`two-provider-global-r56` candidate reached both Provider `READY`, ACK and
`BEFORE_ASSEMBLY` grant verification, then the requester stopped with
`NATIVE_STREAM_FAILED` / `stream event gap exceeded retry budget`. Neither
Provider emitted `ASSEMBLY_STARTED`, `ROOT_VERIFIED`, `RUNNER_READY`, execution,
terminal output or a stage result. Supervisor cleanup passed; resource samples
showed zero swap-I/O delta and the run is not a resource boundary. This is the
first proven post-grant stream-liveness boundary, not a Repo, ORT or model
failure. The repaired candidate now reports authenticated `ASSEMBLY_STARTED`
before root fetch; its immutable scope passed read-only review, rebuilt with
Waf `-j3` in 35.411s, and the C++ lifecycle/assembly suites passed 15/15 and
9/9. A fresh real run with the rebuilt binaries is the next gate; no further
component expansion or timeout increase is planned before that run. See
[r56 evidence](evidence/b189-real-qwen-r56-20260919.md).

**B189-3 progress/heartbeat checkpoint**: 2026-09-19 — the frozen v6
`review-agent` snapshot passed `STATIC_PASS` for authenticated post-Selection
assembly progress. The affected DI/unit/integration/assembly-worker closure
rebuilt from global-r3 with Waf `-j3`; lifecycle (15/15), same-provider
multi-role streamed, D2b streamed, and worker-backed assembly selectors passed.
The first full `Spec175NativeAssembly` run exposed a fixture missing
`metadata.canonicalSourceDigest`/`canonicalSourceBytes`; after read-only
review, the test-only fixture correction rebuilt with `-j3` and the complete
suite passed 9/9. No real Qwen/MiniNDN retry, terminal output, resource drain or
qualification was run in this unit.
See [progress/heartbeat evidence](evidence/b189-r4-progress-heartbeat-20260919.md)
and [failure log](../../docs/failure-log.md).

**B189-3 real Qwen r53 diagnostic checkpoint**: 2026-09-19 — a fresh run with
runtime timing, assignment-fetch tracing, large-fetch timing and dependency
object tracing enabled carried the repaired `900000 ms` dependency budget into
both Providers. Both reached readiness, ACK decisions and `BEFORE_ASSEMBLY`
grant verification; Provider 0 created an assembly staging `root.json`. The
requester still failed with `NATIVE_STREAM_FAILED` / `stream event gap exceeded
retry budget`, while the supervisor recorded `cleanup=PASS` and no remaining
processes. No runner, output, terminal response or completed provider stage
marker was observed; the enabled diagnostics did not expose a completed stage
marker, so this is an observability/liveness boundary rather than proof of
Provider idleness or ORT failure. No qualification is claimed. The next unit
must be a reviewed authenticated progress/heartbeat or bounded admission-state
transition with a C++ counterexample before another real retry. T003/T005/
T006/T007/T009 remain `PARTIAL`. See [r53 diagnostic evidence](evidence/b189-real-qwen-r53-diagnostic-20260919.md).

**B189-3 real Qwen r49-r52 liveness checkpoint**: 2026-09-19 — r49 and r50
stopped at command preflight (provider digest, then tokenizer digest); r51
reached MiniNDN cleanup but used a PATH without `/usr/local/bin`. The corrected
r52 run passed preflight/startup, carried the repaired `900000 ms` dependency
fetch budget into both native Providers, and reached both ACK decisions,
Selection assignment publication, grant verification and post-Selection
assembly staging. The requester still failed before a provider terminal event
with `NATIVE_STREAM_FAILED` / `stream event gap exceeded retry budget`;
`cleanup=PASS`, but no runner, output, terminal response or qualification was
observed. This confirms the timeout wiring is present but leaves a real
stream-liveness versus silent assembly contract. No blind timeout increase is
approved; next work must be a reviewed progress/heartbeat or bounded admission
state transition with a C++ assertion and runtime-timing evidence. T003/T005/
T006/T007/T009 remain `PARTIAL`. See [r49-r52 evidence](evidence/b189-real-qwen-r49-r52-20260919.md).

**B189-3 provider dependency-timeout wiring checkpoint**: 2026-09-19 — the
provider CLI's `--repo-fetch-timeout-ms` is now wired to a dedicated
`dependencyFetchTimeoutMs`; readiness and conversation-control deadlines retain
their independent bounded `fetchTimeoutMs`. Official read-only review of the
final frozen snapshot passed `STATIC_PASS`. The existing global-r3 tree rebuilt
the affected DI library, provider executable and unit-tests with repository Waf
`-j4`; the timeout-budget C++ selectors passed, including an environment
override regression proving that only dependency fetch changes. This closes a
configuration-wiring defect exposed by r48; it does not establish a new Qwen
run, runner, output, terminal response or qualification. T003/T006/T007/T009
remain `PARTIAL`. See [provider timeout evidence](evidence/b189-provider-timeout-20260919.md).

**B189-1c external initializer range fast-path checkpoint**: 2026-09-19 — the
frozen ONNX assembler diff received official read-only `STATIC_PASS`. The shared
external range validator and direct numeric-range digest path preserve exact
identity semantics while avoiding a temporary large `TensorProto` and normalized
copy; INT4/UINT4 retain the prior fallback. The global-r3 unit target rebuilt
with `-j4` in 25.645s and `Spec182OnnxIdentity` passed 13/13. The affected DI,
requester, provider, worker and oracle targets rebuilt with `-j4` in 4m22.913s.
Real Qwen r44 reached both Provider ACK offers and `GRANT_VERIFICATION` with
zero swap-I/O delta, then stopped at `NATIVE_STREAM_FAILED` / provider stream
event-gap; cleanup passed and no terminal response or qualification was observed.
This removes the former r39 preparation-timeout boundary but does not close T003,
which remains `PARTIAL`. See [ONNX fast-path evidence](evidence/b189-onnx-fastpath-20260919.md).

**B189-3 real Qwen r47 diagnostic checkpoint**: 2026-09-19 — a fresh run with
provider assignment/fetch/runtime diagnostics reached the complete post-Selection
entry boundary in one request. Both Providers fetched their assignments and
reached `GRANT_VERIFIED` and `EXECUTION_ENTERED`; Stage/0 reached
`ASSEMBLY_STARTED`, fetched and verified the root and material manifests, and
began the authenticated material-receipt fetch. Stage/1 entered dependency fetch
and retried the absent Stage/0 `hidden_states` manifest. The requester then
expired its stream-event-gap retry budget before any receipt completion, runner
ready marker, stage output or terminal response. Cleanup passed. This narrows the
next boundary to the stream wait/heartbeat contract versus the provider receipt
fetch; it does not prove a receipt or ORT failure and does not close T003/T006/
T007/T009. See [r47 evidence](evidence/b189-real-qwen-r47-20260919.md).

**B189-3 real Qwen r48 liveness checkpoint**: 2026-09-19 — after wiring
`interestLifetimeMs=5000` and `maxEventRetries=8` through the native requester,
a fresh run reached both Providers' ACK/Selection/grant/execution boundary.
Stage/0 verified the root and material manifests and a 3,992,638-byte material
receipt, then continued selected-material assembly without `RUNNER_READY`;
Stage/1 retried the Stage/0 `hidden_states` dependency and failed its terminal
dependency fetch at about 30 seconds. The requester subsequently reported
`NATIVE_STREAM_FAILED`/stream-event-gap. Bundle, candidate, machine, model,
MiniNDN startup and cleanup passed, but workload failed and no output or
qualification was observed. This proves the wait options are carried into the
real request and moves the first observed boundary to long post-Selection
materialization versus the dependency/stream liveness windows; it does not
justify a blind timeout increase or prove receipt/ORT failure. T003/T006/T007/
T009 remain `PARTIAL`. See [r48 evidence](evidence/b189-real-qwen-r48-20260919.md).

**B189-1b r21/r22 chunked-material checkpoint**: 2026-09-19 — external
initializers are published as a bounded header plus ordered raw chunks and are
reassembled by the native post-Selection consumer. Official read-only review
passed for the production/schema, C++ round-trip oracle, and receipt/inline
boundary snapshots (r9/r11/r18). Root Waf `-j4` rebuilt the worker (26.348s),
unit target (31.556s), and affected integration target (24.202s). The C++
chunk round-trip selector and both receipt/selected-fetch selectors passed.
The final combined `unit-tests,integration-tests,di-native-assembly-worker`
Waf build after the last source change completed in 23.498s.
The negative cases include parse-reservation budget exhaustion, receipt
identity/duplicate-key rejection, and oversized inline root rejection before
encrypted fetch. This is a local material/consumer boundary only; T003 remains
`PARTIAL`, and real protected Qwen preparation, ACK/Selection, two-provider
execution, output, drain, MiniNDN and Tiger qualification remain open. See
[chunked material evidence](evidence/b189-material-publication-20260919.md).

**B189-1b r20 local verification checkpoint**: 2026-09-19 — the material
consumer fixture repair passed official read-only review r19
(`STATIC_PASS`, snapshot SHA-256
`92d475dfc3c1bf8a77150c52ac9696dccf4c7ea7d101635f996bceb28e416c6a`). The
affected `integration-tests` target rebuilt with root Waf `-j4` in 22.803s;
the real worker bundle selector and selected-payload budget selector passed.
The related GrantIssuer and protected publisher selectors passed. The Repo
publication selector first exposed a stale assertion that required fewer Repo
objects than payloads; after the r20 review (`STATIC_PASS`, snapshot SHA-256
`448a01c7e7848b46bd23caba73fcac3e7cd10a6ebd7f5466eaa1bf95cbb41aa1`),
`unit-tests` rebuilt with root Waf `-j4` in 26.617s and the Repo, GrantIssuer,
and publisher selectors passed. Repo keeps one independently addressable
range-store object per material payload; protected NDN publication owns bundle
coalescing. The root NDNSF Waf builds only NDNSF-owned targets and consumes
the installed NAC-ABE SDK; NAC-ABE remains owned by its own build system.
This closes only local material-publication/consumer evidence. T003, real
Qwen preparation, ACK/Selection, Provider execution, MiniNDN/Tiger and
qualification remain `PARTIAL`/open. See [material publication evidence](evidence/b189-material-publication-20260919.md).

**B189-1b material-publication checkpoint**: 2026-09-19 — the r5 frozen
material-backed publication diff received official read-only `STATIC_PASS`.
The affected DI closure and the newly registered `spec189-canonical-publisher`
target built with root Waf using `-j4`; the focused material-backed publication
assertion passed 1/1, related publisher regressions passed 5/5, and the
protected request, placement, material, provider-stage and CLI C++ selectors
passed. The existing full publisher suite still has four fixture/environment
failures, which are recorded separately and are not counted as a product PASS.
The previous real r38 run remains stopped at
`PREPARATION_FAILED / DI_NATIVE_PUBLICATION_MATERIAL_LIMIT`; no MiniNDN or
Qwen qualification is claimed. See [material publication evidence](evidence/b189-material-publication-20260919.md).

**B189-1b r39 boundary**: 2026-09-19 — a fresh run using the repaired global
candidate crossed the former publication-byte rejection but stopped at
`PREPARATION_TIMEOUT / DI_NATIVE_PREPARATION_TIMEOUT` during real Qwen
preparation. Cleanup passed; no ACK, Selection, Provider assembly, execution,
terminal response or qualification was observed. T003 remains `PARTIAL`; the
next action is to diagnose and reduce preparation cost before another retry.

**Audit reconciliation checkpoint**: 2026-09-19 — 已将 [DI/Repo static audit](evidence/di-repo-design-static-audit-20260919.md) 与当前源码重新对账：F01 的 material-only consumer 已有局部生产接线和 C++ selector，但真实 protected ingress 仍未验收；F02 已有 focused C++ selector 但实际 ORT/RSS 与完整候选资格仍开放，F08 仍开放，F05/F09 已有 focused C++ selector 但完整候选边界仍开放；F03/F04/F06/F07 分别标为条件性或 legacy follow-up。新增 FR-027..FR-029、T003-R1..R3 与 T009-R1，未将任何任务勾选完成。[audit reconciliation](spec.md#audit-reconciliation--2026-09-19)

**Protected range-store checkpoint**: 2026-09-19 04:52 -05:00 — B189-1a 的冻结范围通过官方只读 `STATIC_PASS`；受影响的 NDNSF targets 用仓库 Waf `-j4` 完成 compile/link，Repo range-store 6/6 C++ cases 和生产 Runtime protected publication 1/1 C++ case 通过，requester `--help` 入口通过。证据记录了 Core ciphertext publication、Repo generation/lease fence、worker cancellation/rollback、bounded reads、source release 和第二次 prepare 无对象增长。[protected range-store evidence](evidence/b189-protected-range-store-20260919.md) 只关闭本地 protected publication 接缝；T003、真实 Qwen、ACK/Selection、Provider、MiniNDN/Tiger 仍为 `PARTIAL`/open。根 NDNSF Waf 只负责 NDNSF 自有目标；NAC-ABE 由其自身构建系统及已安装 SDK 负责（canonical CMake，legacy Waf deprecated），未递归构建。

**B189-2 production-ingress checkpoint**: 2026-09-19 — 在现有 global-r3 Waf tree 对 `integration-tests` 的受影响源以 `-j4` 增量编译成功（39.072s，峰值 RSS 2,114,188 kB，0 swap），并运行真实 C++ `Spec170NdnsfDiCoreFlow` 生产入口 selectors：双 Provider D2b 请求到最终响应、ACK 后 post-Selection runner preparation、篡改 capability 拒绝均通过；post-Selection preparation factory 在手工 ACK/Selection 发布前两个 Provider 都为 0，发布后达到 provider0=2/provider1=1。该组用例证明 production handler 位于真实 ACK/Selection 后路径，但仍未直接记录未选 Provider 的 source fetch/assembly 计数，也未使用真实 Qwen canonical manifest；因此只登记为 T005 的 `FOCUSED_CXX_PASS`，不关闭 T005/B189-2。详见 [production placement evidence](evidence/b189-placement-production-20260919.md)。

**B189-3/T007 rerun checkpoint**: 2026-09-19 — B189-3/T007 的冻结范围经官方只读 `STATIC_PASS` 后，使用正确的 `../waf` 入口在 `build-spec189-b189-3-global-r3` 以 `-j4` 完成六个受影响目标编译链接（16.228s）：`ndnsf-distributed-inference`、`spec185-provider-assembly`、两个 Spec189 oracle CLI/fixture target 和 material oracle。C++ material oracle 15/15、provider-stage oracle 18/18、CLI oracle 5/5 通过；真实 `Provider::serve` selector 通过并记录同一 `preparationId` 的 assembly/ready 配对。首次错误的 `waf` 路径调用已登记在 failure log，未计为产品失败。该出口仍保持 T007 `PARTIAL`：没有独立模型数值、特定 upstream endpoint/compute-start 因果、真实 Qwen 多 token、MiniNDN 或复用资格。[causal oracle evidence](evidence/b189-causal-oracle-20260919.md)

**F02 focused checkpoint**: 2026-09-19 — T003-R1 的 production `MemorySnapshot`/terminal
guard 通过 r2/r3 官方只读复审；仓库根 Waf tree 重新配置并以 `spec189-preparation-memory`
完成 compile/link，修复 build-tree 与 `/usr/local` 同 SONAME 混链后 selector 通过 1/1。
本证据只覆盖分类计数与 post-publication/pre-cache cancellation rollback；实际 ORT allocator/RSS、
真实 Qwen 和完整资格仍开放。[F02 evidence](evidence/b189-f02-memory-20260919.md)

**F09 focused checkpoint**: 2026-09-19 — T003-R3 的 FilesystemRepoStoreBackend
`fsync`/`close`/directory-`fsync` 反例已完成 r4 只读 `STATIC_PASS`、`-j4` 单目标构建和
3/3 C++ selector 运行；F09 仅为 `FOCUSED_CXX_PASS`，不关闭 T003，也不替代 F02/F05
或完整 publication/qualification。见 [F09 evidence](evidence/b189-f09-fd-owner-20260919.md)。

**F05 focused checkpoint**: 2026-09-19 — T003-R2 的 RepoCore mixed quota
selector 完成 r2 只读 `STATIC_PASS`、`-j4` 单目标构建和 2/2 C++ cases；range
reservation 现在同时约束普通 vector 与 exact Data packet admission，abort 后可恢复。
这只关闭 F05 的 focused selector，不关闭 replacement/失败回滚、T003 或 protected
publication/qualification。见 [F05 evidence](evidence/b189-f05-quota-20260919.md)。

**Causal oracle checkpoint**: 2026-09-19 01:37 -05:00 — T007 多次 runner preparation 独立日志 ID 与逐 ID 判据、正常 CLI 的材料/范围/末段 terminal 门已通过 r2 只读任务与组合审查；增量构建 2m54.872s，C++ 材料15例/阶段18例/CLI 5例通过，真实 Provider::serve 接线 selector 通过并记录配对 preparationId。七个输入匹配审查快照；独立模型输出、endpoint 因果、真实 MiniNDN/复用仍未完成，保持 PARTIAL。[causal oracle evidence](evidence/b189-causal-oracle-20260919.md)

**Material oracle checkpoint**: 2026-09-19 01:25 -05:00 — T007 原子材料事件判据已通过只读任务/组合静态门，两个 oracle target 增量构建 23.813s，C++ parser 15 cases PASS；已核对审查/构建源码身份。共享 checker/test 独立归档，CLI 混合改动保留待整合；因果顺序、独立输出、同 handle 复用及真实 MiniNDN 仍未完成。[material oracle evidence](evidence/b189-material-oracle-20260919.md)

**DI/Repo repair design analysis**: 2026-09-19 — 已复核并行新增的 material-only consumer，上一轮 F01 的“未接入”不再代表最新源码；局部 consumer PASS 与真实生产链资格仍区分。完成 [repair design analysis](evidence/di-repo-repair-design-analysis-20260919.md)，建议统一材料读取、存储提交/目录恢复、预算与 turn 所有权，并明确小修复和后续结构收敛的边界。仅分析，未改产品源码、未构建或运行实验、未修改任务完成状态；建议为 `PROPOSED`，不覆盖冻结目标。

**Requester Repo checkpoint**: 2026-09-19 01:23 -05:00 — T003 source owner 首次 external initializer 重复读取已修复，r6 复审及增量构建通过；Repo C++ 5/5，私有 spool 下 Runtime production-entry 1/1。DI 全局安装已完成，DI/Core build/global SHA256 一致，requester 的全局动态链接路径已核对；两个调用方的混合改动尚未归档。真实跨节点读取/Qwen/MiniNDN 保持 PARTIAL。受保护 publication 继续走 Core ServiceUser；详见 [requester Repo evidence](evidence/b189-requester-repo-20260919.md)。

**DI/Repo design audit checkpoint**: 2026-09-19 — 原审计是 13b79ad1 工作树时点的只读扫描，覆盖 372 个源码文件清单、173 个 Python AST 和准备/发布/组装、Repo 目录/容量/持久化、Conversation owner。后续 material-only consumer 已接入局部生产组装路径并通过 C++ selector，因此 F01 的“未接入”只保留为历史边界；准备峰值、混合写入预算、turn owner 和 fd 错误路径仍开放。F03/F04/F06/F07 不属于当前 native protected qualification 的默认调用方，保留条件性/legacy 状态。报告不是逐行全量审查或产品验收，保持 `PARTIAL / NOT_STATIC_PASS`；详见 [DI/Repo design static audit](evidence/di-repo-design-static-audit-20260919.md) 和 [repair design analysis](evidence/di-repo-repair-design-analysis-20260919.md)。

**Updated**: 2026-09-18 19:45 -0500 — the file-backed ONNX assembly/resource subunit passed the r13 read-only static gate and focused validation (`22 passed, 1 skipped`); the real Qwen canonical identity scan passed with 3,689,700 kB peak RSS and zero swaps. The audit retired T008 as a standalone capability task: guard/lifecycle checks are cross-cutting gates owned by T003/T006 and closed by T009. The new single-target global install helper passed static review, its preflight and flags regression suite passed (`100 passed`), and `ndnsf-distributed-inference` built/installed in 11.899s with matching build/global SHA-256 and global ONNX Runtime linkage; no MiniNDN qualification is claimed. Protected Provider ingress, native Repo publication, ACK/Selection, two-provider execution and full cleanup remain open. See [B189 convergence evidence](evidence/b189-convergence.md), [target install evidence](evidence/b189-global-target-helper-20260918.md), [resource evidence](evidence/b189-resource.md), and [failure log](../../docs/failure-log.md).
**Latest checkpoint**: 2026-09-19 — the strict native ONNX source-reuse fix received read-only `STATIC_PASS`; the affected DI/worker/unit targets built in `1m52.201s` with `-j2`, and the five focused ONNX selectors passed after building their worker tools (`11/11`, `5/5`, `9/9`, `11/11`, `30/30`). The combined selector was stopped at a host resource boundary. Real Qwen runs r30 through r33 all stopped at `RESOURCE_BOUNDARY:swapIo` before an interpretable two-provider workload result; MiniNDN/workload remain `NOT_EVALUATED`. r32 used the correct global profile after a separate stale-profile preflight rejection; r33 reached the running/drained sampling phases but still did not start MiniNDN. See [ONNX identity/resource evidence](evidence/b189-onnx-identity-resource-20260919.md).
**Runner-preparation checkpoint**: 2026-09-19 — T006 generation/position metadata binding passed read-only `STATIC_PASS`; global-r3 `unit-tests` rebuilt with `-j1` in `28.600s`, and `NativePreparationContext*` passed 3/3. The regression covers stale adapter metadata removal and authenticated successor/position write-back. This is a metadata unit boundary only; real Provider ingress and ORT execution remain open. See [runner preparation evidence](evidence/b189-runner-preparation-20260919.md).
**Materialization-worker checkpoint**: 2026-09-19 — T006 canonical source/initializer ownership and bounded worker framing passed final read-only `STATIC_PASS` after two review fixes. The affected DI/worker/unit build completed with `-j1`; focused C++ selectors passed `5/5`, `3/3`, `4/4`, and `30/30` (the last with an explicit worker binary directory). This remains a component boundary; protected Repo ingress, real two-Provider execution, output oracle, and drain are open. See [materialization worker evidence](evidence/b189-materialization-worker-20260919.md).
**ONNX memory checkpoint**: 2026-09-19 — the direct-vector source reader and selective ONNX identity/shape materialization passed read-only `STATIC_PASS`. The global-r3 DI target installed successfully with `-j1` in `7m17.389s`; the unit-test target rebuilt in `5m56.704s`; `NativePreparationContext*` passed 3/3, the combined ONNX/assembly selector passed 18 cases, `Spec189*` passed 4 cases, and the selective shape regression passed. Qwen r36 still stopped after both Providers became ready at `RESOURCE_BOUNDARY:MemAvailable` (minimum `1557188608` vs floor `1610612736`, zero swap I/O, peak RSS `4591411200`); no workload or qualification result exists. See [ONNX memory/r36 evidence](evidence/b189-onnx-memory-r36-20260919.md) and [failure log](../../docs/failure-log.md).
**Protected Runtime publication checkpoint**: 2026-09-19 — the new C++ selector passed read-only `STATIC_PASS`; after fixing the fixture spool and bounded Repo read oracle, `spec189-prepared-request` rebuilt from global-r3 with `-j1` in `29.085s`, and the two `Spec189*` cases passed from the repository root. The protected case used Runtime's default Core publisher with `RepoEncryptedLargeDataStore`, verified encrypted source/root/material manifests, per-payload receipt bindings, bounded material reads, source release and no second-prepare object growth. This closes only a local protected-publication boundary; ACK/Selection, Provider assembly, real Qwen execution, output oracle, resource drain and MiniNDN remain open. See [protected Runtime evidence](evidence/b189-protected-runtime-20260919.md) and [failure log](../../docs/failure-log.md).
**Material consumer checkpoint**: 2026-09-19 — the T006 material-only consumer passed final read-only `STATIC_PASS` in r6. The global-r3 `integration-tests` target rebuilt with `-j1` in `3m37.037s`; the C++ selector passed its material-only positive path and aggregate-budget negative path, and the complete `Spec175NativeAssembly` suite passed 8/8 from the repository root. The consumer now reads only the authenticated manifest and selected payloads after Selection, with no source/initializer fallback and a pre-fetch aggregate budget. Production Core ACK/Selection ingress, real Qwen two-Provider execution, output oracle, drain and MiniNDN remain open. See [material consumer evidence](evidence/b189-material-consumer-20260919.md) and [failure log](../../docs/failure-log.md).
**Baseline**: `3e53fec5` plus pre-existing implementation and unvalidated protected-store draft; not a clean qualified candidate.

B189-1a publisher weak-pin、Repo identity fence 和 Core worker/cancel/key release
均已获官方只读 `STATIC_PASS`；组合构建和 package/cache owner selectors 已通过。
剩余的是 protected Repo source owner 在真实 Qwen prepare 中的接线和 B189-1b 原子
材料 consumer，详见 [prepare evidence](evidence/b189-prepare.md)。

T003 源借用修复已静态通过；增量构建 56.712s。初次 native heap corruption 已定位为
installed DI 的旧 ABI（publication 304 vs 360 bytes），全局安装同步后两个 C++ selectors
连续三轮通过。真实受保护 Repo 接线与原子材料仍未完成。见 [prepare evidence](evidence/b189-prepare.md)。

尚无 `QWEN_TWO_PROVIDER_PASS`。r25 run-record 仍 FAIL；Provider 日志已出现
`EXECUTION_ENTERED` / `ASSEMBLY_STARTED`，不能继续称“执行入口完全未观察到”。
尚未证明 runner ready、两段执行、有效终态及资源回收闭合。stream gap 是症状，不能单独认定根因。

本轮 B189-1a 组合构建 342 tasks / 7m8.097s，package-owner selector 3 次、Repo
protected selectors 6 次、bounded publisher 2 次及 Runtime prepare 1 次均 PASS；
保留全局依赖与定向构建、Repo 冷热发布/事务/层 payload fixture、同一 PreparedModel
两请求的 publication-counter selector、placement/cache selector、已注册 C++ 日志 oracle。
它们是组件证据，不能拼成真实 Qwen 全链 PASS。
requester 已有 encrypted range-store 注入草稿，尚未静态/构建/运行验收；
不能注入 plain Repo publisher 替代 protected publication。旧 assembler 仍获取完整 initializer；
分层 producer/consumer 尚未闭合。日志 oracle 的严格事件总序也需修正。

本轮审计见 [audit correction](evidence/spec189-static-audit-20260918.md#architecture-and-progress-correction)。
旧详细 checkpoint 保留在上述 Git 基线和原批次证据；本表取代十任务线性调度。
T001 映射已关闭；host direct/launcher guard 已复审，39 host checks 通过；
两个 C++ 目标 -j4 构建成功，4 个 lifecycle 用例三轮通过。真实路径 native counters
及完整资源回收仍待验证；资源门由 T009 收口，未运行完整模型。磁盘现约 34 GiB 可用。
文档修订已获冻结 v2 的 DOCUMENTATION_STATIC_PASS；11/11 技能入口、6 task ID、
29 FR、链接/锚点及 diff 检查通过，详情见上述审计记录。产品验收保持 PARTIAL。

## Execution Progress

| Unit / Details | Status | Depends | Evidence / Remaining | Updated |
| --- | --- | --- | --- | --- |
| [T009-R258 Chained multi-turn KV](evidence/b189-r258-multiturn-kv.md) | PARTIAL | r257 cache diagnostic | 定向构建/回归通过；真实第二轮KV复用成功，第三轮父状态过期，非Provider切换；cleanup PASS；保留期限与placement优先待修，不替代完整Repo和同handle验收 | 2026-09-22 |
| [T009-R260 Request-scoped runner reuse](evidence/b189-r260-runner-reuse.md) | PARTIAL | r259 performance baseline | C++45/1042三次GREEN；三轮32token相同、KV恢复、准备70→6、C++缓存诊断/清理PASS；跨request/完整资格/文档动态门仍开放 | 2026-09-22 01:55 -05:00 |
| [T009-R261 Resident ONNX sessions](../190-multiturn-latency/tasks.md#t005-resident-session) | PLANNED | r260; loaded/request evidence separation | 转Spec190 T005承接；尚未实现，不再维护第二套驻留实施任务；本Spec资格不关闭 | 2026-09-22 02:20 -05:00 |
| [T009-R259 Conversation affinity and retention](evidence/b189-r259-affinity-retention.md) | PARTIAL | r258 first boundary | C++43/882与6/147各三次通过；真实三轮10/11/11 EOS、后两轮KV恢复、C++ CACHE_DIAGNOSTIC_PASS、cleanup PASS；binding/文档/完整Repo/同handle资格未关闭 | 2026-09-22 01:22 -05:00 |
| [T005-R257 Core assignment order](evidence/b189-r257-core-assignment-externalization.md) | PARTIAL | T005 / r256 first boundary | Core八场景163 assertions三次PASS；全局安装核验与真实两Provider10token/EOS缓存诊断PASS；完整Repo、sanitizer、文档交付待完成 | 2026-09-21 23:59 -05:00 |

6 项是能力任务，不按数量计算产品百分比。T003 三个独立执行出口见
[bounded execution units](batch-execution.md#bounded-execution-units)，B189-1a 已关闭，
当前下一步为 B189-1b。
本轮下一步是 T006/T007 的 r162 真实重跑：r155 已证明 system-wide source cache
identity/hash/size reuse，r156 已通过 source protobuf release 越过先前 assembly
memory boundary，r161 已定位并修正 local ONNX adapter 的 core lineage validation
边界。affected DI closure 已完成编译和全局安装；r162 必须先观察是否越过 local
materialization，再继续 Provider-1 assembly、handoff、terminal、output、drain 和
warm/repeat。资源门仍保持原阈值，不能用 cache hit 或 focused selector 代替资格。
T006-R1 的 admission→ROOT 单调序列与 T006-R2 的 progress allowlist 已是 focused
C++ 出口，不再重复领取。
每个出口验证后立即记录，不等整项 T003 写完才第一次构建。
本轮新增 memory-lifecycle audit 已关闭两个局部 working-set 出口：prepare 的完整
source/initializer 在 material manifest 固定后可释放，Provider role model 物化后可
释放 selected material；取消/rollback 反例也已用 C++ 屏障和 drain 通过。DI/selector
增量构建通过，定向 selector 全部通过；完整 selector 仅剩 real-Core fixture 的 `/tmp`
staging `Permission denied`，因此不改变 T003/T006/T009 的 PARTIAL 状态，也不把环境失败
记为产品 PASS。[memory audit](evidence/b189-memory-lifecycle-static-20260920.md)
host guard 和小型 lifecycle safety entry 已达到其前置出口；剩余 native counter
接入属于 T003/T006 的实际 owner，最终在 T009 以完整采样和 drain 证据收口，不再
创建重复的资源行政批次。
本轮文档 v2 已获 DOCUMENTATION_STATIC_PASS；结构/29 FR/链接与技能同步检查通过，
见 [follow-up verification](evidence/spec189-static-audit-20260918.md#follow-up-verification)。
受保护接缝已通过 B189-1a 静态门、受影响目标构建及 C++ focused selectors；Repo
adapter producer/consumer 和 Runtime protected publication 的本地出口已记录，
但 protected Provider consumer/真实 Qwen 链路仍未审查，本轮未构建或运行模型。

| Unit / Details | Status | Depends | Remaining exit / Evidence |
| --- | --- | --- | --- |
| [T001 Freeze integration boundary](#t001) | DONE | — | 2026-09-18 13:29 -0500：真实接线/候选/后继缺口及五 lane 映射已只读审查；仅关闭实施边界。[convergence](evidence/b189-convergence.md) |
| [T003 Prepare and reuse Repo materials](#t003) | PARTIAL | T001 | material-backed publication budget/receipt path, focused C++ selectors and system-wide canonical source-cache hash/size reuse pass locally; r155 observed source-cache write/hit in the real chain. Protected range-store/material-only consumer ingress, full F02 ORT/RSS attribution, F05 replacement/rollback, F09 complete publication boundary and final two-request reuse remain open. [material publication](evidence/b189-material-publication-20260919.md); [prepare](evidence/b189-prepare.md); [r155 source cache](evidence/b189-r155-source-cache-memory-boundary-20260920.md) |
| [T005 Authenticate placement](#t005) | PARTIAL | T003 | ACK 后规划、signed Selection、生产 ingress handler/no-fetch 计数。[placement](evidence/b189-placement.md); [production ingress](evidence/b189-placement-production-20260919.md) |
| [T006 Materialize selected ranges](#t006) | PARTIAL | T005 | material-only C++ consumer、aggregate budget、Selection-scoped admission/progress selectors、source release、role-boundary rebuild 和 ONNX assembly worker 已通过局部验证；native install 已更新，但下一次 MiniNDN 仍需验证 Provider-0/Provider-1 runner、handoff、terminal、owner/cancel counters 和 full warm path。[material consumer](evidence/b189-material-consumer-20260919.md); [stage boundary](evidence/b189-stage-materialization-unit-20260921.md); [r156 boundary](evidence/b189-r156-worker-release-lineage-boundary-20260920.md) |
| [T007 Validate handoff and output](#t007) | PARTIAL | T006 static gate | lineage core/edge validation 修正已编译并安装；r162 尚未验证 NDN endpoint 因果、hidden-state handoff、独立输出与 terminal response。[causal oracle](evidence/b189-causal-oracle-20260919.md); [r161 boundary](evidence/b189-r161-lineage-validation-diagnostic-20260920.md) |
| [T009 Qualify reuse and repeat](#t009) | PARTIAL | T003 + T005 + T006 + T007 | r161 resource guard/cleanup passed but stopped at local lineage validation; same-handle two requests, independent repeat, F08 generation-guard, native counters, terminal output and qualification remain open。[convergence](evidence/b189-convergence.md); [r161 boundary](evidence/b189-r161-lineage-validation-diagnostic-20260920.md) |

## Task checklist

- [x] T001 [US1] Freeze the remaining production integration and candidate boundary.
- [ ] T003 [US1] Connect topology-independent Qwen preparation, Repo publication and reference-only reuse.
- [ ] T005 [US2] Verify real ACK-driven planning and authenticated Selection at production ingress.
- [ ] T006 [US4] Fetch selected Repo materials, assemble bounded native CPU runners, and expose native resource counters.
- [ ] T007 [US3] Validate real handoff, causal events and independent terminal output.
- [ ] T009 [US5] Qualify the resource envelope, complete MiniNDN path, same-handle reuse and independent repeat.

## Audit follow-up registry

这些是现有能力任务下的稳定子出口，不是新的行政任务，也不改变 Spec189 的六项
能力任务计数。每项都必须有 C++ production target/selector 和独立失败边界；未完成
时保持所属 T 项 `PARTIAL`。

| Subtask | Finding | Owner / dependency | Status | Exit evidence |
| --- | --- | --- | --- | --- |
| T003-R1 | F02 preparation peak | Runtime/ONNX preparation owner; before T009 full model | FOCUSED_CXX_PASS / QUALIFICATION_OPEN | C++ selector records source/material/encryption/ORT budget categories and post-publication cancel/retry cleanup; actual ORT allocator/RSS and real Qwen remain open; [F02 evidence](evidence/b189-f02-memory-20260919.md) |
| T003-R2 | F05 mixed quota reservation | RepoCore range/vector/Data packet admission; before protected candidate | FOCUSED_CXX_PASS | C++ mixed range/vector/Data selector passed; replacement/failure rollback and protected candidate remain open; [F05 evidence](evidence/b189-f05-quota-20260919.md) |
| T003-R3 | F09 fd error ownership | FilesystemRepoStoreBackend error path; before protected candidate | FOCUSED_CXX_PASS | injected fsync/close failure, one-owner/no-duplicate-close and preserved manifest boundary; [F09 evidence](evidence/b189-f09-fd-owner-20260919.md) |
| T003-R4 | cache-compatible metadata-only receipt must suppress Core root prefetch while retaining authenticated assignment identity | `NativeCanonicalArtifactPublisher` → `NativeRequestPlanner` → Core assignment preparation → Provider assembler; before next cache-compatible MiniNDN retry | IN_PROGRESS / STATIC_PENDING | r166 first boundary is preserved in [r166 evidence](evidence/b189-r166-cache-root-identity-boundary-20260920.md); implementation is in the working tree, pending static review, affected build/install, and fresh guarded runtime |
| T006-R1 | executable assembly progress sequence | NativeProvider executable runner factory; before the next real Qwen retry | FOCUSED_CXX_PASS / QUALIFICATION_OPEN | executable uses the Selection-scoped sequence shared with admission, and a C++ reporter-contract regression uses two independent reporters with admission sequence 1 followed by ROOT sequence 2; no real MiniNDN credit until the fresh run crosses the boundary; [T006-R1 evidence](evidence/b189-t006-r1-progress-sequence-20260919.md) |
| T006-R2 | cross-provider stream progress binding | Core `ServiceUser`/`InvocationStream` collaboration consumer; before the next real Qwen retry | FOCUSED_CXX_PASS / QUALIFICATION_OPEN | consumer allowlists exact `{provider, providerSelectionDigest, role operationId}` tuples and monotonic freshness; local lifecycle/status selectors pass, while r161 remains the latest installed-candidate lineage boundary and r162 must verify the post-fix stream path; [progress heartbeat evidence](evidence/b189-r4-progress-heartbeat-20260919.md); [r161 boundary](evidence/b189-r161-lineage-validation-diagnostic-20260920.md) |
| T009-R1 | F08 turn owner race | Conversation/PreparedModel handle installation; after T007 and before same-handle PASS | PLANNED | C++ barrier interleaving where older terminal/exception cannot overwrite or close newer turn |
| F03/F04 follow-up | catalog snapshot/history gap | Only if catalog snapshot/delta becomes a candidate caller | DEFERRED | snapshot-required + incarnation/oldest-sequence C++/Python sync evidence |
| F06/F07 follow-up | compatibility replacement/durability | Legacy helper/backend maintenance, not current native protected path | DEFERRED | separate compatibility task and failure model; no Spec189 PASS credit |

## Retired task IDs

合并不代表完成，旧 ID 不再单独领取或勾选为 PASS。

| Former ID | Disposition | Preserved obligation |
| --- | --- | --- |
| T002 | MERGED_INTO T003 | Qwen graph/config/digest 验证、原子层/shared 材料生成、staging cleanup |
| T004 | MERGED_INTO T003 | reference-only、两请求无新增发布、stale/released/oversized negatives |
| T010 | MERGED_INTO T009 | 独立重复、候选一致性、最终 verdict 与文档交付 |
| T008 | MERGED_INTO T009 | host guard、受控 stop、native resident/runner counters、完整采样与 drain；小型 guard/lifecycle 证据保留在 resource record |

<a id="t001"></a>
## T001 — Freeze integration boundary

**Read**: 最新 failure-log/raw boundary、架构阅读集、当前 Spec、global dependency receipt。
**Write**: `evidence/b189-convergence.md`；不另建 gate 框架。

1. 用 CodeGraph 固定 Runtime::prepare→RepoSourceProvider→DI_NativeRequester→
   NativeCanonicalOnnxAssembler→NativeProviderHandler/NativeEpochCoordinator 的实际接线与缺口。
2. 标明生产 CLI、独立 C++ assertion target、实际 Waf target/源码闭包；
   核对已有全局 ABI/receipt，只有缺失/改变/不兼容才安装或重建。
3. 固定 model revision、动态 KV schema、服务角色/key registry、拓扑、资源阈值、
   摘要派生入口和 handoff endpoint/attempt/sequence。最终 binary hash 在批末更新，
   不要求尚未实现的验收先通过。

**Acceptance**: 五 lane 的已验/待改/待测映射可执行，无循环依赖；
不因文档修改/run-id 改变重新全量构建，不授予产品 PASS。

## Retired T008 — cross-cutting gate

T008 不再作为独立能力任务。它保留的义务由实际 owner 承担：T003/T006 在各自
selector 中交付 native resident/materialization/lease/runner counters 和取消回收
反例，T009 在每次 full-model run 前调用现有 host guard，并在成功或分类停止后
统一检查采样、child 状态和 drain。`evidence/b189-resource.md` 保留已有 39 个
host checks、lifecycle fixture 和真实 identity 记录；这些证据不单独授予产品 PASS。

资源门规则：阈值来自 immutable profile，超过阈值必须分类为 `RESOURCE_BOUNDARY`；
只清理本 run staging，保留原始日志和有效 Repo 对象证据；`finally`/`kill` 本身
不等于 lifecycle PASS。由于这部分不新增生产能力，不再为它单独建批次或重复编译。

<a id="t003"></a>
## T003 — Prepare and reuse Repo materials

**Write**: `Runtime.cpp`、`NativeCanonicalPreparationCatalog.*`、
`NativeCanonicalArtifactPublisher.*`、定义 NativeCanonicalSource 的
`NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp`、
`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoSourceProvider.hpp`、
`examples/DI_NativeRequester.cpp`；已有 Repo/PreparedModel C++ selectors；
`evidence/b189-prepare.md`。按实际定义路径修改，不复制 Qwen API。

共享 protected 接缝另涉及 `ndn-service-framework/ServiceUser.{hpp,cpp}`、
`EncryptedLargeDataRangeStore.hpp` 和 Repo `RepoEncryptedLargeDataStore.hpp`。
先按 B189-1a 验证 protected Repo 接缝，再按 B189-1b 实现原子材料；Core 不依赖
DI/Repo 类型。T003 不负责 ACK/Selection ingress，该边界属于 T005。
`NativeCanonicalArtifactPublisher::CacheState::prepared` 当前只保留 receipt 和 weak
serving pins；`PreparedModelPackage`/活动 request 才是强 owner。复用
ModelPreparationCache 预算/淘汰作为唯一保留策略，publisher 不成为第二个无界强 owner，
活动 package/request 仍保证可读。B189-1a 只验证受保护接缝和真实 package/cache owner
反例；B189-1b 才冻结原子材料 schema，不把未来 schema 当作当前 API。
C++ 反例必须走真实 publisher→package→淘汰路径，不能只手动 reset Core token。
B189-1a 当前 worker/cancel/key 与 Repo identity 静态门、组合构建和受保护
package/cache owner runtime selector 已通过；这只关闭本地 protected publication
接缝，不能把它写成 T003 完成。
具体出口与反例见 [bounded commit](contracts/model-preparation.md#bounded-commit-and-identity-ownership)。

**Audit follow-up exit B189-1c**：T003-R1 记录 source/initializer/material/encryption/ORT
各类 peak owner 与取消/重试清理；T003-R2 让 range reservation、普通 vector/Data packet
写入和 replacement 共用逻辑 quota；T003-R3 对 fsync/close failure 验证单一 fd owner。
三个出口必须使用 C++ production selector，分别记录首个失败边界，不能由 Python
脚本或磁盘剩余空间检查替代。

1. pinned canonical graph/initializer 生成拓扑无关原子层与 shared tensor 引用。
   层→节点/权重范围由 graph 推导并校验；embedding/final/tied weights 按内容去重。
   不能把预导出的 [0,14)/[14,28) 最终模型当 prepare 格式。
2. 复用 Repo manifest/payload owner/文件后端，最小版本化扩展 layer→graph/tensor
   object 或受验证 byte-range 的 digest/size/依赖关系。读单元有界，
   发布不再累积整份 vector；旧 schema 明确拒绝或走受测兼容路径，不能静默降级。
3. 将实际 requester 接入 Runtime Repo publisher/source 生命周期；对象持久化、
   manifest commit 且正常 Repo 读取可达后才 READY。source owner 可释放，
   服务与活动 lease 存活；不可达/对象丢失明确失败，request 不隐式重发模型。
4. 同一 immutable identity 的重复 prepare 命中已有 manifest/reference，且不会
   固定 Provider placement；同 handle 的两次请求和 publication/ingest 零增量由
   T009 的真实 MiniNDN 资格统一证明，避免在 prepare selector 中重复模拟最终链路。
5. 复用已通过 selector，仅补 real-Qwen receipt、源释放、stale manifest、
   digest/range/schema、staging rollback 和必要 envelope negatives。
   离线 snapshot→canonical 导出可复用，但不替代 native prepare/Repo。

**Acceptance**: C++ production entry 验证真实 Repo 材料、commit/可达性、源释放和
immutable prepare lookup。完整 Qwen run 由 T009 先通过资源门；此处不要求两
Provider 执行成功，也不重复证明最终同 handle 两请求。F02/F05/F09 的 B189-1c
出口未通过前，不能把完整候选标为 ready。

<a id="t005"></a>
## T005 — Authenticate placement

**Write**: NativeRequestPreparation/Envelope、必要 Core/NativeProviderHandler 接线、
`tests/integration-tests/spec189-placement-oracle.t.cpp`；
`evidence/b189-placement.md`。

1. 真实 ACK offers 后由 planner 决定两个覆盖范围；profile 可约束 [0,14)/[14,28)，
   不能改变 prepare manifest 或注入假 ACK/Selection。
2. 使用已有 typed projection/签名/canonical grant identity builder，
   绑定 manifest、Provider、role/range、attempt/epoch、plan digest。
3. 生产 Selection ingress 验证无 offer、stale epoch/digest、未选 Provider、
   overlap/out-of-range；实际 fetch/runner factory 计数为零。
4. 修正现有 selector 的 CPU backend/ABI 和 synthetic grant fixture；
   保留其组件价值，禁止用 cache-layer hit 证明网络授权已验。

**Acceptance**: 真实 signed path 与生产 ingress C++ assertions 通过；
无效选择无重型副作用，不依赖 T006 模型执行形成循环。

<a id="t006"></a>
## T006 — Materialize selected ranges

**Write**: NativeCanonicalOnnxAssembler、NativeOnnxAssemblyWorker、provider/Repo adapter、
已有 assembly/ownership selectors；`evidence/b189-execution.md`。

1. 实际 consumer 使用 T003 同一 manifest/receipt，授权后读取选定层与显式 shared
   tensors；替代整 initializer 下载再切片，未接线不能隐式回退旧路径。
2. 有界读取/文件物化与 RAII lease；digest/size/range/role/manifest 验证后才进 ORT。
   记录实际 fetched bytes、resident buffers、mapped files、runner owner，
   不能只用 cache.stats 或 RSS 代替源对象释放证明。
3. 错包/取消/组装失败测试无半成品 runner、文件/lease/worker 泄漏；
   保留已验 endpoint-preservation 回归，不重新实现。
4. 不可变内容可缓存复用，每请求重验授权；KV/会话与 runner 生命周期分离，
   不强制 warm request 重建 runner，不长期持有整源。

**Acceptance**: 实际 provider factory 用 Repo 选中材料构造 CPU runner，
没有每 Provider 整模型临时副本；共享 bytes 与失败/回收出口可核对。

<a id="t007"></a>
## T007 — Validate handoff and output

**Write**: 必要 NativeProviderHandler/NativeEpochCoordinator 修复、
`examples/Spec189TwoProviderOracle.cpp` 与已有 C++ handoff fixtures；
`evidence/b189-execution.md`。

1. 真实 Core/NDN hidden-state handoff 核对 endpoint digest、attempt/model/role/
   sequence、shape/dtype；不直接跨节点传内存对象。
2. 按 [placement contract](contracts/placement.md) 修正事件 checker：
   model assembly 与 upstream fetch 可交错，首段没有 upstream dependency；
   authorization/runner/input 均就绪才 execute，末段响应后所有 owner drain。
3. 覆盖成功、cache-hit、缺事件、错误因果/identity 的最小 C++ fixture。
   生产 CLI 和日志 checker 均不单独证明模型正确。
4. 冻结短输入及独立 reference，以 C++ 检查 shape/finite、
   冻结 logit tolerance 或 top-token 期望；digest 只作身份。
   保留 cancel/provider stop/stale handoff 反例，不扩张质量或 KV 性能工程。

**Acceptance**: 原生 handoff 与独立输出判据可执行，checker 不误拒合法次序且拒绝异常；
最终真实 Qwen 资格仍由 T009 负责。B189-3 的 assembly-admission progress 已有稳定
C++ 出口；在下一次真实候选运行前，不再加入新的组件职责或仅为减少重试而调整超时。

<a id="t009"></a>
## T009 [US4, US5] — Qualify resource envelope, reuse and repeat

**Write**: 维护的 MiniNDN runner、单进程 C++ requester/driver 复用入口及证据 checker；
raw logs 放唯一 `.codex-tmp` run 目录；`evidence/b189-convergence.md`；
失败同步 failure-log。

1. 在 T003/T005/T006/T007 runtime 出口通过后，冻结实际源码内容、global ABI、模型/
   manifest、profile/topology、binaries/oracle；自动派生摘要，检查 policy/key/module/disk。
2. 两 CPU Provider，native prepare→Repo→ACK→planner/Selection→按需组装→
   NDN handoff→有效输出→drain。必须由一个长期存活的 C++ requester/driver
   在同一 Runtime/PreparedModel handle 上先 prepare 一次，再提交两个独立
   request；不能用脚本启动两个独立 requester 进程来替代。publication 增量为零，
   两个结果均通过独立 C++ oracle。
3. 原始证据落盘后保持同 candidate，用新 run-id 重复上述场景；
   request id/key/临时路径属于 run identity，不使 candidate 摘要变化。
4. 每次运行先通过 host guard，再比较 fetched bytes/cache/runner、RSS/Repo resident/
   物化峰值和 post-drain baseline。
   warm cache 可复用 runner；不能为凑计数而强制重建。
5. 失败保留第一已证实边界/未知部分，修复复审受影响范围再复测。T007 必须先
   提供独立固定输入的 C++ numerical/output oracle；token schema、digest 或
   CLI 日志不能替代它。
   实现引起的 API/行为变化按 Design/MANAGEMENT.md 同步契约/PDF/文档交付。
6. T009-R1 用 C++ 屏障控制旧 turn terminal/exception、新 turn start 和 handle
   installation；generation 不匹配时旧 turn 不得覆盖 active owner 或被 close 取消。
   该门通过前不能把“同 handle 两请求”仅凭两个结果文件认定为复用 PASS。

**Acceptance**: 两独立运行均成功，且各自同 handle 两请求/输出/资源/drain 齐全，
才 `QWEN_TWO_PROVIDER_PASS` / [x]。classified failure 不算完成。无 SIF/Tiger/27B 工作。

## Logical Batches and Dependencies

### Current Checkpoint — r140 focused secure-erase candidate

The v4 immutable source snapshot passed read-only `STATIC_PASS` after the
protected-source erase, lease-ordering, and range-backed material repairs. The
affected native closure then built `556/556` with Waf `-j4`; focused C++
selectors passed material `2/2`, canonical publisher `14/14`, ONNX activation
`9/9`, and protected-directory cleanup `1/1`. Six affected targets were
installed and their dependency closure was checked with `ldd`. The first
publisher selector failure against the default `/tmp` staging directory was a
permission boundary; the rerun with a private `0700` staging directory passed.
Evidence: [r140 focused build and secure-erase](evidence/b189-r140-focused-build-secure-erase-20260920.md).
This is not a MiniNDN/Qwen or qualification result; T003, T005, T006, T007,
and T009 remain `PARTIAL`.

The subsequent real installed MiniNDN run `two-provider-global-r140` passed
the host resource gate, ACK closure, Selection commit, both Provider
Selection acceptance records, and both `GRANT_VERIFICATION` boundaries. It
reached Provider-0 assembly start and Provider-1 dependency fetch, then
Provider-1 exhausted 356 exact signed-data attempts with `error=deadline`
before Provider-0 published the requested tensor manifest. The run was then
operator-stopped while Provider-0 was still materializing; supervisor cleanup
passed. This is a classified upstream-readiness/deadline boundary, not a
resource, authorization, Repo, ORT, numerical-output, or qualification PASS.
Evidence: [r140 MiniNDN exact-fetch boundary](evidence/b189-r140-minindn-exact-fetch-boundary-20260920.md).
T003, T005, T006, T007, and T009 remain `PARTIAL`; the next repair must
review the initial dependency deadline/progress contract and use a new run ID.

### Current Checkpoint — r141 initial producer-readiness repair

The r140 boundary was reduced to a precise C++ regression: a consumer started
the first exact V3 manifest fetch before the producer had published that
manifest, while the existing fetch budget also applied the post-publication
`noProgressDeadlineMs`. The new
`V3DependencyIoWaitsForInitialProducerReadiness` integration case first failed
against the old implementation after the delayed producer publication
(`critical check published has failed`, selector rc `201`). The production
repair now bounds that first manifest fetch by the request hard deadline and
dependency fetch budget; subsequent segment fetches retain the
`noProgressDeadlineMs` bound.

The affected `integration-tests` target rebuilt `127/127`. The new regression
then passed, and the existing V3 manifest/segment case plus the new case passed
as a two-case selector with `No errors detected`. Raw build and selector output
is retained under
`.codex-tmp/spec189-r141-initial-readiness-regression/`; the immutable static
review snapshot is under
`.codex-tmp/spec189-r141-initial-readiness-regression/static-review-20260920/`.
This is a focused C++ repair only: no new installed candidate or real r141
MiniNDN run has yet observed manifest publication, runner readiness, terminal
output, numerical oracle, repeat, or qualification. T003, T005, T006, T007,
and T009 remain `PARTIAL`.
The frozen source review returned `STATIC_PASS` with no blocker. Its P2
follow-ups remain unobserved: an unpublished manifest must terminate at the
hard deadline/fetch budget, a smaller fetch budget must cap initial readiness,
and a post-manifest segment stall must retain the no-progress failure. These
are coverage improvements, not a qualification result.

### Current Checkpoint — r142 owned-swap resource boundary

The fresh installed-binary MiniNDN run `two-provider-global-r142` passed the
host gate at startup and reached both Provider `READY` states, signed ACK
offers, selection-assignment publication, and both `GRANT_VERIFICATION`
records at `BEFORE_ASSEMBLY`. Provider-0 also created an active assembly
staging root. Before any observable `EXECUTION_ENTERED`, `DEPENDENCY_FETCH`,
`RUNNER_READY`, terminal output, or numerical oracle, the maintained host
guard stopped the run at `RESOURCE_BOUNDARY:ownedSwap` after its owned-swap
limit was exceeded. Supervisor cleanup was `PASS`; requester cancellation and
Provider-1 socket EOF are shutdown consequences. The raw run and resource
trace are retained in [r142 evidence](evidence/b189-r142-owned-swap-boundary-20260920.md).

This is a host resource boundary, not a protocol/Repo/ORT/model result or
qualification PASS. T003, T005, T006, T007, and T009 remain `PARTIAL`. The
next real run requires a new run ID and a host state that stays below the
owned-swap guard before retrying the post-Selection path.

### Current Checkpoint — r143 MiniNDN routing-entry review and owned-swap boundary

The fresh r143 installed-binary run reached both Provider `READY`, signed ACK
offers, and `GRANT_VERIFICATION` at `BEFORE_ASSEMBLY`, then the unchanged host
guard stopped it at `RESOURCE_BOUNDARY:ownedSwap` with
`ownedSwapBytes=268681216` against `268435456`. Cleanup passed. No Selection,
execution, runner, terminal response, numerical oracle, repeat, or qualification
result exists; T003, T005, T006, T007, and T009 remain `PARTIAL`. Evidence is
in [r143 routing and owned-swap evidence](evidence/b189-r143-routing-plan-owned-swap-20260920.md).

The MiniNDN entry was then reviewed against the upstream NLSR and static-routing
examples. Because Spec189 requires deterministic application-prefix routes, the
entry now uses `Nfd + NdnRoutingHelper` only, removes the duplicate NLSR owner,
retains `--nlsr-wait-s` as a compatibility alias for `--routing-wait-s`, and
writes a validated node-to-APP plan. `py_compile`, `--help`, the node-plan
helper, and `git diff --check` passed. A five-node MiniNDN smoke then confirmed
that the static route `/spec189/smoke` published by `memphis` is visible in
`neu`'s FIB and that cleanup passes. This corrected entry has not yet been used
for a new real Qwen run. The read-only review-agent
`01a0c116-f576-76c3-bc5e-d322e2d4ae59` reviewed base `4ff5b700` to checkpoint
`c03260d2`, found no P0/P1/P2 issue, and returned `STATIC_PASS`; only optional
P3 dependency/FIB diagnostics remain.

The first fresh run with this corrected entry, `two-provider-global-r144`,
started the five-node MiniNDN topology and reached both Provider `READY`,
signed ACK offers, and `GRANT_VERIFICATION` at `BEFORE_ASSEMBLY`. It then
stopped at `RESOURCE_BOUNDARY:ownedSwap` with
`ownedSwapBytes=271720448` against `268435456`; cleanup passed. No requester
Selection, assembly, execution, terminal response, numerical oracle, repeat,
or qualification result exists. Evidence is in
[r144 static-routing owned-swap evidence](evidence/b189-r144-static-routing-owned-swap-20260920.md).
T003, T005, T006, T007, and T009 remain `PARTIAL`; the next run must use a
new run ID and a host state that stays below the unchanged resource limits.

执行顺序：`B189-0 → B189-1 → B189-2 → B189-3 → B189-5`。
保留历史 ID 稳定链接；序号不再代表时间。
成员、五 lane、动态检查、唯一结果记录见 [batch-execution.md](batch-execution.md)。
达到批次出口即验证，不为了少编译加入新职责。

### Current Checkpoint — r147 SmolLM2-135M external-data staging-name boundary

r147 使用 external-data canonical source 后仍在 MiniNDN 前的 identity 预检
停止：ONNX 内部 external location 为
`canonical-smollm135m-external-initializer.bin`，而 launcher 的固定 staging
文件名是 `requester/canonical-initializer.bin`，因此未能打开 initializer。
external graph 的 ONNX checker/ORT 已通过；没有进程、ACK、Selection、Provider
执行、资源或资格结果。该边界是候选 staging-name mismatch，不是 ONNX
输入/KV/输出契约缺陷，不新增 adapter。详见
[r147 evidence](evidence/b189-r147-smollm135m-external-name-boundary-20260920.md)。
下一次只统一 external location 与 launcher staging 名称，并使用新 run ID。

### Current Checkpoint — r146 SmolLM2-135M inline-material publication boundary

r145 的 tokenizer digest 格式修正后，r146 已通过候选预检并真正启动
MiniNDN；Requester 在本地 `prepare` 失败于
`DI_NATIVE_PUBLICATION_MATERIAL_PAYLOAD_TOO_LARGE`。supervisor 未触发资源门，
cleanup 为 `PASS` 且无残留进程；尚未出现 ACK、Selection、Provider assembly/
execute、terminal 或 oracle。原因是当前 539,094,828-byte inline canonical
ONNX 的 initializer material payload 超过现有 1 MiB bounded publication
limit，不是 ONNX 输入/KV/输出契约不匹配，因此不新增 adapter。详见
[r146 evidence](evidence/b189-r146-smollm135m-inline-material-boundary-20260920.md)。
T003、T005、T006、T007、T009 继续保持 `PARTIAL`；下一次仅切换为 external-data
canonical source、绑定 initializer digest，并使用新的 run ID。

### Current Checkpoint — r145 SmolLM2-135M tokenizer preflight boundary

首次以现有 `llama` profile 准备 `HuggingFaceTB/SmolLM2-135M`：完整
canonical ONNX 为 30 层/8081 nodes，输入、30 层动态 KV、`float32 logits`
输出均通过 ONNX checker、ORT session 和直接执行；两阶段 profile manifest、
EOS `[0]`、15/15 layer split 及 8081-node 精确 mapping 也已生成。完整
MiniNDN 尚未启动。r145 在 launcher 预检因 tokenizer digest 缺少 `sha256:`
前缀停止，未观察到 NFD、ACK、Selection、Provider assembly/execute、terminal
或资源结果；详见 [r145 evidence](evidence/b189-r145-smollm135m-tokenizer-preflight-20260920.md)。
该边界不是 ONNX 输入/KV/输出契约不匹配，不新增 adapter。T003、T005、T006、
T007、T009 继续保持 `PARTIAL`；修正格式后必须用新 run ID 重试。

### Current Checkpoint — r148 SmolLM2-135M Core stream-gap boundary

r148 使用与 launcher staging 名称一致的 external-data canonical graph，沿用
现有 `llama` profile；候选 ONNX 输入、动态 KV、logits 契约没有新增 adapter
需求。真实 MiniNDN 已到达两 Provider `READY`、签名 ACK 和
`GRANT_VERIFICATION BEFORE_ASSEMBLY`，随后 Requester 因
`NATIVE_STREAM_FAILED: stream event gap exceeded retry budget` 停止。没有
Selection、assembly、execution、runner、terminal、oracle、repeat 或资格结果。
supervisor cleanup 为 `PASS`，owned-swap 未触发。详见
[r148 evidence](evidence/b189-r148-smollm135m-stream-gap-20260920.md)。

当前工作单元转向 Selection 后的 immutable compatible cache：必须保持授权和
Selection 为前置门，命中后才可跳过 layer fetch/copy/assembly，并用精确 digest
和 role/model identity 校验 cache。T003、T005、T006、T007、T009 继续保持
`PARTIAL`；cache 命中测试通过也不等于 Spec189 multi-provider qualification
完成。

### Current Checkpoint — stable cache focused selector

新增的 `ProductionAssemblerCacheScansStableRootAndVerifiesFileDigest` 已通过：
它从非 run-scoped stable root 读取已有 manifest/model，流式校验模型 digest，
命中后返回现有路径；篡改模型后返回 miss。组合 selector 中旧的
`ProductionAssemblerCacheColdHitUsesExactArtifact` 因未设置
`NDNSF_SPEC182_BIN_DIR=build-spec189-oracle`，在 worker fixture 发现前停止；
worker 实体存在，详见 [selector evidence](evidence/b189-cache-selector-worker-path-20260920.md)。
这不是 cache 实现失败。下一步以显式 worker 环境重跑旧 assembler cache case，
再验证 Provider 生产 wiring；T003、T005、T006、T007、T009 仍为 `PARTIAL`。

### Current Checkpoint — r149 stable-root production wiring boundary

r149 使用重建并安装到系统的 standalone `di-native-provider`、worker、
requester 和 authority；Provider cache root 为固定的
`/var/tmp/ndnsf-di-native-artifacts/`，Provider 子目录由 identity SHA-256
派生。两 Provider 都到达 `READY`、签名 ACK 和
`GRANT_VERIFICATION BEFORE_ASSEMBLY`，但约三分钟没有 Selection 完成、
`CACHE_HIT`、`ASSEMBLY_STARTED`、execution、terminal 或 oracle 事件，遂按
精确 PID 受控停止。该 catalog 使用 `protection_epoch=epoch-1`，因此新的
plaintext stable-cache loader 正确不复用 protected grant-bound ciphertext；
stable root 没有 assembled model。详见
[r149 evidence](evidence/b189-r149-smollm135m-stable-cache-boundary-20260920.md)。
这不是 cache、资源或 ONNX 契约失败；T003、T005、T006、T007、T009 继续保持
`PARTIAL`，下一步仍需先解除 Selection 前 stream/admission boundary，再做
真正 warm-hit 对照。

### Current Checkpoint — r150 Qwen3-0.6B EOS preflight boundary

r150 在 MiniNDN 启动前停止于 `MODEL_EOS_TOKEN_IDS_REQUIRED`。使用的
`stage-manifest-qwen-r99.json` 缺少显式 `eosTokenIds`，而 pinned Qwen3-0.6B
snapshot 的 `config.json` 给出 `eos_token_id=151645`、`<|im_end|>`；现有
`stage-manifest-qwen-v2.json` 已包含该 stop contract。没有 ACK、Selection、
Provider assembly/execute、cache、资源、模型输出或资格结果，supervisor
cleanup 为 `PASS`。详见
[r150 EOS preflight evidence](evidence/b189-r150-qwen-eos-preflight-20260920.md)。
这不是 ONNX input/KV/output 契约不匹配，不新增 adapter；下一次使用新 run ID
和 `stage-manifest-qwen-v2.json` 重试。T003、T005、T006、T007、T009 继续
保持 `PARTIAL`。

### Current Checkpoint — r151 Qwen3-0.6B build identity preflight boundary

r151 仍未进入 MiniNDN：使用已安装 `/usr/local` binaries 时把 `/usr/local`
误作为 build identity root，后置复核失败于
`BUILD_RECEIPT_MISSING:/usr/local/spec180-native-build.json`。该复核发生在
1,503,264,768-byte canonical initializer 已复制到 run directory 之后；没有
ACK、Selection、Provider assembly/execute、cache、资源、模型输出或资格结果，
supervisor cleanup 为 `PASS`。详见
[r151 build receipt evidence](evidence/b189-r151-qwen-build-receipt-preflight-20260920.md)。
下一次保留 `build-spec189-oracle` 作为 build identity root，显式提供
`spec180-native-build.json` digest，同时继续使用 `/usr/local` installed
binaries；并将 identity 复核前移到大文件 materialization 之前。T003、T005、
T006、T007、T009 继续保持 `PARTIAL`。

### Current Checkpoint — r152 Qwen3-0.6B Selection boundary

r152 使用 `stage-manifest-qwen-v2.json` 和正确的 `build-spec189-oracle`
receipt identity，预检通过后进入真实 wired MiniNDN。两个 Provider 均到达
`READY`、签名 `ACK_DECISION status=1` 和 `GRANT_VERIFICATION
BEFORE_ASSEMBLY`；没有 Selection、`CACHE_HIT`、assembly、execution、terminal
或模型输出，Requester 在约三分钟观察窗口后受控取消。227 个 resource samples
中最低可用内存约 4.70 GiB，`ownedSwapBytes` 峰值约 5 MiB，supervisor cleanup
为 `PASS`；stable root 只有 active assembly metadata，没有 assembled model。
详见 [r152 Selection evidence](evidence/b189-r152-qwen-selection-boundary-20260920.md)。
该边界属于 `ACK/grant verification → Selection` admission/stream liveness，
不是内存、磁盘、ownedSwap、cache 或 Qwen ONNX 契约失败。T003、T005、T006、T007、
T009 继续保持 `PARTIAL`；下一步先取得 Selection wire/consumer 证据，再做 stable
root cold/warm cache 对照。

### Current Checkpoint — r155 Qwen3-0.6B source-cache / assembly memory boundary

r155 验证了新的系统唯一 source cache：preflight 为该模型 identity 创建
`/var/tmp/ndnsf-di-native-artifacts/model-source/<identity-hash>/`，并在本轮完成
content-addressed graph 与 1,503,264,768-byte initializer 的 Repo 写入；run-local
initializer 与输入保持同 inode hardlink，没有再创建第二份物理 canonical source Repo。
真实链路随后到达 `ACK_CLOSED`、`SELECTION_COMMITTED`、两 Provider 的 authenticated
Selection、`GRANT_VERIFIED`，Provider-0 到达 `ASSEMBLY_STARTED`，Provider-1 到达
`DEPENDENCY_FETCH`。host guard 在首次 material fetch/assembly 峰值停止于
`RESOURCE_BOUNDARY:MemAvailable`：最低 available 为 `1370591232` bytes，低于
`1610612736` 门限；`ownedSwapBytes` 峰值 `157200384`，仍低于
`268435456` 门限，cleanup 为 `PASS`。因此 source cache 实现与定向测试通过，但
Provider assembled cache 尚未完成，不能声称 warm hit 或完整推理；没有
`RUNNER_READY`、terminal、模型输出或资格 PASS。详见
[r155 source-cache evidence](evidence/b189-r155-source-cache-memory-boundary-20260920.md)。
T003、T005、T006、T007、T009 继续保持 `PARTIAL`；下一步先针对 assembly 内存峰值
和 cache 可复用边界做静态/定向诊断，不重复运行同一必然触发的冷 assembly 路径。

### Current Checkpoint — r156 worker-release / GenerationEpochLineage boundary

r156 命中 system-wide source cache，且使用释放完整 source protobuf 后重新构建、安装的
native DI closure。真实链路已到达 `ACK_CLOSED`、`SELECTION_COMMITTED`、两 Provider
authenticated Selection、`GRANT_VERIFIED`、Provider-0 `ASSEMBLY_STARTED` 和
`RUNNER_READY`；Provider-1 进入 `DEPENDENCY_FETCH status=begin`。本轮没有触发 host
resource boundary：最低 `MemAvailable=2326757376`，最高
`ownedSwapBytes=205512704`，supervisor cleanup 为 `PASS`。

Provider-0 随后在 generation lineage 输出边界失败于
`GenerationEpochLineageV1 invalid producerRole`，Requester 最终报告
`NATIVE_STREAM_FAILED`。没有 terminal response、model output、oracle、repeat 或
qualification PASS；详见 [r156 evidence](evidence/b189-r156-worker-release-lineage-boundary-20260920.md)。
因此 memory working-set 修复只记为 focused/production-boundary progress，T003、T005、
T006、T007、T009 继续保持 `PARTIAL`。下一步仅检查 V3 projected activation edge 与
planner-owned `TOKEN_FEEDBACK` edge 的 sealed producer/consumer binding，修复后使用新
run ID 重跑；不把 provider-1 的后续停止误判为独立根因。

### Current Checkpoint — r157 diagnostic launcher preflight boundary

r157 原计划验证 Provider worker 的 fail-closed producer-role 诊断，但启动命令漏传
tokenizer digest 的 `sha256:` prefix，在 MiniNDN/NFD 启动前停止于
`MODEL_TOKENIZER_DIGEST_MISMATCH`。没有 process、ACK、Selection、assembly、lineage、
execution、terminal、resource 或 model-output 结果；raw run 已保留，详见
[r157 evidence](evidence/b189-r157-tokenizer-preflight-boundary-20260920.md)。这次失败
不是代码或 ONNX contract 结果；修正参数格式后使用新的 run ID 重试，T003、T005、T006、
T007、T009 继续保持 `PARTIAL`。

### Current Checkpoint — r158 bootstrap key-generation boundary

r158 已正确命中 source cache，但在 MiniNDN/NFD 启动前创建第一个 bootstrap
identity 时停止。诊断命令设置了非法的 `NDNSF_NDN_LOG=info`；launcher 原样传为
`NDN_LOG=info`，ndn-cxx parser 报 `malformed logging config: '=' is missing`，
`ndnsec key-gen` 以 `134`（`SIGABRT`）退出。隔离的 root key-gen 复现了该行为，
移除非法日志值后成功；因此本轮没有 process、ACK、Selection、assembly、lineage、
execution、terminal、resource、model-output 或 qualification 结果，不能用于判断
lineage 修复。监督器 cleanup 为 `PASS`，raw run 已保留，详见
[r158 evidence](evidence/b189-r158-bootstrap-keygen-boundary-20260920.md)。
下一步只修正运行参数为合法的 `NDNSF_NDN_LOG='*=TRACE'`，使用新的 run ID
继续验证已安装的 fail-closed producer-role diagnostic；T003、T005、T006、T007、
T009 继续保持 `PARTIAL`。

### Current Checkpoint — r159 coordinator lineage boundary

r159 使用合法的 `NDNSF_NDN_LOG='*=TRACE'`，成功越过 r158 bootstrap key-gen，命中
verified source cache，并进入真实 MiniNDN。两个 Provider 都到达
`EXECUTION_ENTERED`；Provider-0 到达 `ASSEMBLY_STARTED`、`RUNNER_READY`，并记录
ONNX warmup `session_cache=hit`；Provider-1 进入 `DEPENDENCY_FETCH`。Provider-0
随后仍在 `GenerationEpochLineageV1 invalid producerRole` 处 terminal failure，
Requester 报 `NATIVE_STREAM_FAILED`。新加的 `ProviderRoleWorker` fail-closed 检查
没有触发，故首个失败位于其后的 `NativeEpochCoordinator::lineageForEdge` 路径；
尚未知道具体 projected `TOKEN_FEEDBACK` edge 字段，不能填默认 role。supervisor
记录 `boundary=null`、`cleanup=PASS`、无残留进程；514 个样本最低
`MemAvailable=1848680448`，最高 `ownedSwapBytes=59289600`，最低磁盘剩余
`9280036864`。没有 terminal、输出、oracle、repeat 或 qualification PASS，详见
[r159 evidence](evidence/b189-r159-lineage-coordinator-boundary-20260920.md)。
下一步只在 `lineageForEdge` 增加 edge-describing fail-closed 诊断，重建/安装受影响
DI target 后用新 run ID 继续；T003、T005、T006、T007、T009 继续保持 `PARTIAL`。

### Current Checkpoint — r160 lineage validation boundary

r160 使用合法的 `NDNSF_NDN_LOG='*=WARN'`，source cache 命中，真实 MiniNDN 到达
两个 Provider `EXECUTION_ENTERED`、Provider-0 `ASSEMBLY_STARTED`/`RUNNER_READY` 和
Provider-1 `DEPENDENCY_FETCH`。Provider-0 仍返回裸的
`GenerationEpochLineageV1 invalid producerRole`，Requester 随后返回
`NATIVE_STREAM_FAILED`。ProviderRoleWorker 与 `NativeEpochCoordinator::lineageForEdge`
两个新增 edge 检查都没有改变错误，故还需覆盖 `extractGenerationEpochLineage()`
内部的 `GenerationEpochLineageV1::validate()`；本轮不填默认 producer role。
supervisor 为 `boundary=null`、`cleanup=PASS`、无残留进程；385 个样本最低
`MemAvailable=2569969664`，最高 `ownedSwapBytes=31666176`，最低磁盘剩余
`7732269056`。没有 terminal、输出、oracle、repeat 或 qualification PASS，详见
[r160 evidence](evidence/b189-r160-lineage-validation-boundary-20260920.md)。
下一步只在最终 lineage validate 边界增加 request/epoch/consumer/index 诊断，重建/安装
DI target 后用新 run ID 继续；T003、T005、T006、T007、T009 继续保持 `PARTIAL`。

### Current Checkpoint — r161 local ONNX lineage validation boundary

r161 使用合法的 `NDNSF_NDN_LOG='*=WARN'`、已安装的受影响 DI closure 和 verified
system-wide source cache，真实 MiniNDN 已到达两个 Provider 的
`EXECUTION_ENTERED`，Provider-0 的 `ASSEMBLY_STARTED`/`RUNNER_READY`，以及
Provider-1 的 `DEPENDENCY_FETCH`。Provider-0 随后返回带 request/epoch/consumer/index
诊断的 `GenerationEpochLineageV1 invalid producerRole`，Requester 返回
`NATIVE_STREAM_FAILED`。源码核对确认首个失败边界是
`OnnxRuntimeModelRunner::materializeCausalPositionInputsV1()` 对尚未绑定 edge-local
role 的初始 core lineage 调用了 full `validate()`；这不是 ONNX contract、cache 或
resource failure。已保留 r161 raw run，详见
[r161 evidence](evidence/b189-r161-lineage-validation-diagnostic-20260920.md)。
已实现 `validateCore()` 并只让本地 ONNX position materialization 使用它，wire
encode/decode 和 edge publication 继续使用 full `validate()`；受影响 DI targets
已重建并安装，详见下一条 build/install checkpoint。T003、T005、T006、T007、T009
继续保持 `PARTIAL`。

### Current Checkpoint — r161 core-lineage fix build/install

已将 `GenerationEpochLineageV1::validateCore()` 接入 local ONNX causal-position
materialization；`encodeGenerationEpochLineage()`、decode 和 edge publication 仍调用
full `validate()`。affected closure 使用 system compiler/binutils 和 `-j4` 完成
`ndnsf-distributed-inference,di-native-provider` 构建，Waf 报 `build finished
successfully (7m56.905s)`；随后 `scripts/install-global-target.sh` 分别成功安装两个
target，且没有重建 Core/Repo/UAV。该共享头变更使 DI installable closure 的 196 个
Waf tasks 重新编译，属于受影响闭包而非全仓库重编。尚无 r162 runtime 结果，T003、
T005、T006、T007、T009 继续保持 `PARTIAL`；下一步用新 run ID 验证 local
materialization 后的 Provider-1 assembly、handoff 和 terminal 边界。

### Current Checkpoint — r162 controlled cancellation before cache-compatible mode

r162 命中 verified system-wide source cache，并在真实 MiniNDN 中到达
`ACK_CLOSED`、`SELECTION_COMMITTED`、两 Provider 的 `GRANT_VERIFIED`/
`EXECUTION_ENTERED`、Provider-0 `ASSEMBLY_STARTED` 和 Provider-1
`DEPENDENCY_FETCH status=begin`。为避免继续运行普通 Repo material fetch/assembly
造成无必要的工作集增长，本轮被有意中断；supervisor `cleanup=PASS`、无残留进程，且
host resource guard 未触发（minimum `MemAvailable=7148163072`、maximum
`ownedSwapBytes=118554624`）。没有 `RUNNER_READY`、terminal、model output、oracle、
repeat 或 qualification PASS，详见
[r162 cache-compatibility stop evidence](evidence/b189-r162-cache-compatibility-stop-20260920.md)。

因此这不是新的协议失败根因，也不能计为完整链路进展。下一工作单元只实现显式、默认关闭、
hash/size 校验、缺缓存即 fail-closed 的 temporary cache-compatibility mode；它只替代
Selection 之后的 Repo material fetch，不绕过 ACK、Selection、grant 或 placement 校验。
实现后必须先完成静态审查，再编译/安装并使用新 run ID 验证；T003、T005、T006、T007、T009
继续保持 `PARTIAL`。

### Current Checkpoint — cache-compatibility implementation static gate

已完成 temporary cache-compatibility mode 的最小实现：`di-native-provider` 只在显式
`--cache-compatibility-source-dir` 下启用；launcher 只在显式
`--cache-compatibility-mode` 且 system-wide source cache 命中时传入该参数。requester
在同一显式模式下使用 metadata-only authenticated publication receipt，不创建或配置
run-scoped encrypted Repo；source owner 仍独立校验 plain graph/initializer。Provider
仍先完成 authenticated Selection/grant/placement，随后由 assembler 校验 cache identity
schema、model/manifest digest、文件大小和 SHA-256；普通 Repo fetcher 在该模式不调用，
缺失、不匹配、material-backed source 和 protected role 均 fail closed。默认路径和正式
Repo contract 未改变。

实现后的静态门已通过：Python `py_compile`、受影响五个实现文件的 `git diff --check`、
参数传播/normal-versus-compatibility 分支审查均 `PASS`。随后只重建受影响 DI closure：
`./waf build --targets=ndnsf-distributed-inference,DI_NativeRequester,di-native-provider -j4` 报
`build finished successfully`（210 tasks，`rc=0`）；`ndnsf-distributed-inference` 和
`DI_NativeRequester` 的 global install 均 `rc=0`；未重建 Core/Repo/UAV。安装态
`DI_NativeRequester --help`、`ldd` closure 和 Provider flag 检查也通过。持久记录见
[cache-compatibility static evidence](evidence/b189-r163-cache-compatibility-static-20260920.md)。
这不是 runtime 或 qualification PASS；下一步必须使用新 r164 run ID 运行，再按
`implement → static check → runtime → bug fix → static check` 继续。

### Current Checkpoint — r163 cache-compatibility disk boundary

r163 使用显式 `--cache-compatibility-mode`、已安装 affected DI closure 和 verified
system-wide source cache 启动两个 Provider；两方均记录
`repoFetch=skipped-after-selection`，但在 ACK/Selection 之前由 host guard 停止于
`RESOURCE_BOUNDARY:diskFree`。监督器 `cleanup=PASS`、无残留进程；约 1.4 GiB 已写入
run-scoped encrypted Repo，`MemAvailable` 和 `ownedSwapBytes` 未触发门限。该证据说明
当前实现只跳过 Provider post-Selection Repo fetch，不能避免 requester prepare 阶段的
`NativeCanonicalArtifactPublisher` 大体积 protected publication；不是 ONNX/KV/output
契约结果，也不是 cache identity 失败。详见
[r163 disk boundary](evidence/b189-r163-disk-boundary-20260920.md)。

因此不重复运行同一命令。下一工作单元若要继续，必须先做 requester-side 最小诊断接缝
设计/实现，使 authenticated prepare/manifest/ACK/Selection 身份仍可验证而不发布大体积
protected payload；实现后重新静态审查，再用新的 run ID 运行。若不改变该边界，则需要
另行授权清理足够的失败运行大文件或提供有足够空闲空间的文件系统。T003、T005、T006、
T007、T009 继续保持 `PARTIAL`。

### Current Checkpoint — r164 requester cache-compatibility disk boundary

r164 已使用 requester metadata-only publication path、显式
`--cache-compatibility-mode`、verified system-wide source cache，并且未传入
`--encrypted-repository-path`。host guard 在 admission、requester/Provider 启动之前停止：
`diskFreeBytes=4232839168 < minDiskFreeBytes=4294967296`，`availableBytes=9151160320`、
`ownedSwapBytes=0`；cleanup=PASS、无残留进程，且没有创建新的 run-scoped encrypted Repo。
因此本次没有 ACK、Selection、grant、placement、assembly、materialization、terminal、
output、oracle、repeat 或 qualification 结果。这是宿主机磁盘安全门，不是 requester
receipt、协议或 ONNX contract 失败。详见
[r164 disk-boundary evidence](evidence/b189-r164-requester-cache-disk-boundary-20260920.md)。
在获得足够磁盘空间或完成明确授权的失败运行大文件清理前，不得重复同一命令；T003、T005、
T006、T007、T009 继续保持 `PARTIAL`。

### Current Checkpoint — r236 concrete-boundary alias diagnostic

r236 的名称级 raw run 已确认 Provider-0 不是停在模型准备黑箱：它完成了
`WORKER_DONE` 和 `CACHE_FINALIZATION_DONE`，随后在旧的 activation-output alias
校验处失败。当前 Qwen 冷组装边界实际产生四个 concrete activation names，而
planner 的 `hidden-layer-13-to-14` 只是 logical authenticated edge；旧代码把两者
错误地当成同一 tensor contract。对比旧 Python wrapper 示例后，已将修复方向收敛为
logical edge + authenticated concrete `bundleTensorNames`，并把共享 control inputs
作为 bundle passthrough；不再进行多对一 alias。r236 raw/evidence 见
[r236 concrete-boundary evidence](evidence/b189-r232-preparation-alias-contract-20260921.md)。

当前仍没有 `RUNNER_SPEC_READY`、`RUNNER_READY`、ORT、terminal 或 qualification
PASS，T003、T005、T006、T007、T009 继续保持 `PARTIAL`。下一步是 affected C++
build/install、projection/assembly/Provider selectors 及安装态 hash 核对；通过后
使用新 run ID 验证两 Provider handoff 和 terminal，失败则先更新本 checkpoint。

普通 Waf install 的第一次尝试在覆盖 `/usr/local/lib/libndnsf-distributed-inference.so`
时因 shell 权限失败，未形成可运行 installed candidate；失败边界已登记在
[bundle-contract evidence](evidence/b189-r232-preparation-alias-contract-20260921.md)。
这不是实现或 runtime 失败，下一步只重试仓库已有的 global-target 安装入口，随后
运行 selectors。

selector 的第一次调用误用了 `build-spec189-oracle/tests/` 路径并返回 shell `127`，
没有测试进程启动；Waf 产物实际在 `build-spec189-oracle/spec189-preparation-alias`。
该 invocation 边界已记录，下一步使用实际路径执行 focused selectors。

正确路径下 preparation selector 已通过 `4` cases；Provider selector 的 3 个
assembly integration cases 因测试默认路径未包含 `build-spec189-oracle` 而返回
`201`，candidate worker 已确认存在。该环境定位问题不计为源码失败；下一步只设置
`NDNSF_SPEC182_BIN_DIR=build-spec189-oracle` 重跑 Provider selector。

修正环境变量后 `spec185-provider-assembly` 已通过 `20/20`；同时
`spec189-preparation-alias` `4/4`、`spec189-canonical-publisher` `16/16` 通过。
这只是 focused selector PASS，未推进任何完整链路 checkbox；下一步核对 installed
candidate hash，再使用新 run ID 验证 `RUNNER_READY`、ORT、跨 Provider handoff、
terminal 和 cleanup。

### Current Checkpoint — r237 initial dynamic KV shape boundary

r237 使用新的 raw run
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r237-bundle-member-contract`
进入真实 MiniNDN，并确认 bundle-member contract 已越过旧 alias 边界：Provider-0
完成 `RUNNER_SPEC_READY` 和 `RUNNER_READY`，Provider-1 完成
`DEPENDENCY_FETCH status=begin`。首个生产失败发生在 Provider-0 首次 ORT execute 的
`/Add_2`：`Attempting to broadcast an axis by a dimension other than 1. 3 by 4`。
profile 显示首轮 KV 为 `[1,8,1,128]`，与旧 Python 示例中
`_qwen_initial_state()` 对 `past/cache/sequence` 维生成 `0` 的契约不一致；这不是
model preparation、cache、内存或 handoff 失败。supervisor cleanup 为 `PASS`、无残留进程、
resource guard 未触发。持久证据见
[r237 initial KV shape evidence](evidence/b189-r237-initial-kv-shape-boundary-20260921.md)。

已在 C++ runner 中修复：仅对 epoch-0 缺省 state 保留 dynamic zero，普通输入、显式
shape metadata 和 warmup 不改变；streamed initial state 同步使用该规则。当前仍无
terminal success、model output、oracle、repeat 或 qualification PASS，T003、T005、
T006、T007、T009 保持 `PARTIAL`。下一步按
`implement → static check → runtime → bug fix → static check` 完成受影响 DI build、
focused regression，再使用新的 raw run 验证真实 Qwen two-provider 链路。

### Current Checkpoint — r238 exact Data wire-size boundary

r238 已越过 r237 的首轮 KV shape 失败：Provider-0 到达 `RUNNER_READY` 并完成
实际首轮 ORT execute，Provider-1 到达 `DEPENDENCY_FETCH status=begin`。新的首个
失败是 Stage0→Stage1 exact `NDNSF_DATA_V1` 发布：底层记录
`contentBytes=7784 wireBytes=8999 limit=8800`。原因是当前 C++ 默认
`maxSegmentSize=7600` 没有为长 V3 Data name 和签名预留足够 wire 空间；旧 Python
provider 使用 `max_segment_size=7000`。这不是 model output、KV、cache 或内存失败，
cleanup 为 `PASS`、无残留进程。持久证据见
[r238 exact Data wire evidence](evidence/b189-r238-exact-data-wire-boundary-20260921.md)。

已将 C++ Provider handler/dependency IO 默认统一为 `7000`，并保留 exact publisher
最终 wire-size fail-closed 检查。当前没有 Provider-1 execute、terminal、oracle、
repeat 或 qualification PASS，T003、T005、T006、T007、T009 保持 `PARTIAL`。下一步
按 `implement → static check → runtime → bug fix → static check` 完成受影响 build、
focused regression，再用新的 raw run 验证 exact publication、跨 Provider fetch、
terminal 和 cleanup。

### Current Checkpoint — r239 historical Python/C++ state-and-lineage boundary

r239 在 r237/r238 修复后完成 Provider-0 `RUNNER_READY`，Provider-1 完成
`DEPENDENCY_FETCH(begin, complete)`，但 Provider-0 在 decode-state commit 前报
`Provider role is missing decode-state output: present_key.0`，Provider-1 报
`native epoch coordinator is missing its canonical token input`。对照旧 Python
Qwen 实现及 Git 历史（`96c26cab`, `78f27d64`, `51d928f2`, `f7f8fc89`, `5264ebc4`,
`76e8556d`）确认：worker 必须在发布 state-stripped handoff 前保留本地 state，且
`PIPELINE` handoff 必须承载并校验 generation lineage。r239 raw/evidence 见
[r239 Python/C++ contract evidence](evidence/b189-r239-python-cpp-contract-boundary-20260921.md)。

当前没有 terminal、oracle、repeat 或 qualification PASS；T003、T005、T006、T007、
T009 继续保持 `PARTIAL`。下一步仅实现上述两个受影响 C++ contract 修复，先完成
static review、affected build 和 focused regression，再用新的 raw run 重试真实
MiniNDN 链路；失败后先更新本 checkpoint，不得重复 r239。

### Current Checkpoint — r240 protected dataflow authorization boundary

r240 使用新的构建和新的 raw run 验证了 r239 的两个 C++ 修复：source-cache 校验、
authenticated Selection、两端 `GRANT_VERIFIED`、Provider-0 cache hit、
`RUNNER_READY` 以及 Provider-1 `DEPENDENCY_FETCH(begin, complete)` 均已通过。
Provider-0 随后在第一次依赖读取的 `ProtectedRuntime::authorizeDataflow(Fetch)`
失败，terminal reason 为 `protected dataflow is not authorized for this role/endpoint`。
cleanup=PASS、无残留进程，但没有 terminal success、oracle、repeat 或 qualification
PASS；T003、T005、T006、T007、T009 继续保持 `PARTIAL`。证据见
[r240 protected dataflow evidence](evidence/b189-r240-protected-dataflow-boundary-20260921.md)。

这次失败尚未足以判断是 endpoint digest、provider/role peer map、或请求输入边界的
构造错误；下一步只在新的 raw run 打开 `NDNSF_DI_PROTECTED_DATAFLOW_DIAGNOSTIC=1`
和 runtime timing，记录 `allowed/owns_role/peer_matches` 的首个拒绝字段，随后按
`implement → static check → runtime → bug fix → static check` 修复并重新验证。

### Current Checkpoint — r241 application-input decode boundary

r241 的诊断确认 Provider-0 在 epoch 0 使用 request-backed `APPLICATION_INPUT`
后，decode epoch 仍尝试 fetch 同一 edge；诊断为 `producer=`、`peer_present=0`、
`allowed=0`、`owns_role=1`。这解释了 r240 的授权失败：ProtectedRuntime 的
inter-Provider binding 没有错误地收录该 ingress edge，错误在 epoch role 构造。
证据见 [r241 application-input evidence](evidence/b189-r241-application-input-decode-boundary-20260921.md)。

已实现 epoch>0 移除 `APPLICATION_INPUT`、保留 `TOKEN_FEEDBACK`，并把该场景加入
C++ coordinator regression；`git diff --check` 通过。但包含旧
`NativeArtifactBinding` 聚合赋值的其他 unit test 使 `unit-tests,integration-tests`
构建在无关文件处失败，新增 regression 尚未编译/运行。T003、T005、T006、T007、
T009 继续保持 `PARTIAL`；下一步不修改无关 stale test，只完成可用 affected build、
regression verification，再用新的 raw run 重跑真实链路。
