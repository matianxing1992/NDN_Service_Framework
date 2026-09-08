# Native Token Stream Design

**Status**: ALGORITHM_DEFINED / T001 IN_PROGRESS / implementation NOT_STARTED
**Scope**: CD-006, CD-007 / A7-08 / T007, T011, T016

2026-09-07追加Production Caller Inventory和Requester Acceptance and Recovery Resolution，关闭本契约原先的stream调用方/接受边界设计疑问。下文早期OPEN语句描述其提出时状态，以后两节的具体处置为准；A7-08产品实现/验证和O-004其余工作仍OPEN。

## Artifact Identity

2026-09-07只读取tokenizer，不运行模型。交付清单`Experiments/TigerCluster/jobs/spec175/workload.json`指定Qwen/Qwen3.6-27B revision `6a9e13bd6fc8f0983b9b99948120bc37f49c13e9`。从该[固定工件地址](https://huggingface.co/Qwen/Qwen3.6-27B/resolve/6a9e13bd6fc8f0983b9b99948120bc37f49c13e9/tokenizer.json)读取12,807,982 bytes，SHA256 `5f9e4d4901a92b997e463c1f46055088b6cca5ca61a6522d1b9f64c4bb81cb42`与清单一致；decoder为ByteLevel，add_prefix_space/trim_offsets/use_regex均false。tokenizers0.20.3实际加载成功；好/🙂/é/中/U+FFFD的IDs分别95887/169171/933/95789/5513，单token完整decode正确；单token不是stream反例。

本地另一Qwen工件`results/_artifacts/qwen-tokenizers/sha256/832b6a8c551fa4418c5bdd745b014da0b72ac65ff912b3ba73ae9f79ab0c3d9d/tokenizer.json`是11,422,650 bytes，文件SHA256 `be75606093db2094d7cd20f3c2f385c212750648bd6ea4fb2bf507a6a4c55506`，ByteLevel三参数均true。该目录artifact摘要不是tokenizer.json文件摘要，两者均不替代交付版本身份。ByteLevel这三个参数用于pre/post processing，不改变其decode_chain字节转换；依据[固定HF源码](https://github.com/huggingface/tokenizers/blob/v0.20.3/tokenizers/src/pre_tokenizers/byte_level.rs)。

## API and Ownership

在T007的`cpp/adapters/qwen/tokenizer-bridge/src/lib.rs`及`tokenizer-abi.h`新增私有ABI `ndi_token_decode_stable(const void* handle,const uint32_t* ids,size_t count,uint8_t skipSpecial,uint8_t final)`，返回既有NdiTokenResult。沿用原decode的ID验证、flags仅0/1、1Mi token/16MiB输出限额、panic/错误映射及同库free。原五函数不删除；它们的旧ABI探针不被追溯改成stream验证。

`NativeTokenizer.hpp/.cpp`新增`decodeStable(const vector<int64_t>& ids,bool skipSpecialTokens,bool final) const -> string`，复用Impl mutex/handle/result RAII。ids是完整候选生成前缀；final=true返回与完整decode逐字节相同的文本，false只返回不会被后续token改写的完整稳定前缀。此方法不签名、不读journal、不发布事件，不额外创建可变Rust stream句柄。算法临时buffer属于调用，异常不污染共享tokenizer；并发请求只共享只读模型及既有串行锁。

Rust owner缓存已解析decoder profile及ByteLevel字符到byte映射；不持有会话ID、token历史或已发布文本。用于stream的profile验证发生在factory/准备阶段，空ids调用也验证profile，不等到第一个token才发现未知pipeline。既有null、WordPiece及单独ByteFallback、ByteLevel明确支持；其他Sequence/Replace/Strip等组合必须另有对应算法证明才可宣称支持stream，不影响其完整encode/decode可用性，不静默退回Python或终态一次性输出。当前已核对Qwen及三份既有fixture均在支持范围。

## Stable Prefix Algorithms

| Profile | final=false | final=true |
| --- | --- | --- |
| null decoder | 原HF完整decode；过滤special后以空格连接，已输出部分稳定 | 原完整decode |
| WordPiece | 原HF完整decode；按原token位置/prefix/cleanup逐token处理，不自行实现WordPiece | 原完整decode |
| ByteFallback | 过滤special后暂存最后未闭合byte run，完整decode此前已闭合部分；规则见[generation boundary](native-generation-design.md#verified-boundary-and-planned-bytefallback-algorithm) | 原完整decode包括最后run及原replacement语义 |
| ByteLevel | 按官方byte alphabet还原各token，拼接字节；保留尾部未完成UTF-8序列，已确定有效或无效部分按Rust标准库lossy语义输出 | 原完整decode，包括尾部replacement |

ByteLevel转换必须逐token执行：若token所有字符都在byte alphabet，映射全部字符；任一字符不在alphabet，则整个token按原UTF-8 bytes加入，不混合转换。special先按原选项过滤。用`std::str::from_utf8`识别有效前缀；error_len=Some(n)表示已确定无效子段，按`String::from_utf8_lossy`语义输出并继续；error_len=None表示尾部未完成，暂存至下一次或final。不得删除合法U+FFFD。不借助输出字符末尾是否U+FFFD猜测输入字节是否完整。

调用采用完整candidate IDs重算以保留事务隔离；既有实现本就逐token完整decode。本阶段不增加增量缓存和另一套恢复状态，之后若优化必须保持同一结果/事务边界。ByteFallback可能暂存整个连续byte run，ByteLevel仅暂存不完整尾序列，两者不能共用“最多3 bytes”的规则。

## Epoch Integration and Recovery Boundary

T011在NativeEpochCoordinatorConfig保留完整`textDecoder`用于stop/final判断，增加同工件owner的`stableTextDecoder(ids,final)` callback。NativeStandaloneTokenizer factory及NativeProviderHandler配置共同传递两种callback，均捕获同一shared_ptr<NativeTokenizer>；确切所有调用方与注册表由O-004收口，不能只接CLI。requireTextOutput=true时两者必须齐全，禁止默默把完整decode当stable。

epoch对candidate IDs先完整decode以计算现有stop suffix，加EOS/MAX_TOKENS判断final；再取得稳定candidateText并验证其包含已提交generatedText前缀，生成textDelta。终止时stable结果必须等于完整结果，事件中的最后delta包含flush文本，不新增无token的伪事件。事件接受前只持有局部candidate，拒绝/取消丢弃局部candidate；接受后延续既有反馈/运行态提交顺序，不为解码迁移擅改Core协议。

恢复重算既有committedPrefixTokenIds，每个位置按相同decoder profile、flags与当时非终止规则重建稳定文本，原有token相等检查和重复事件抑制保留。已发布文本不能回滚。源码NativeProviderHandler的eventSink把publishStreamEvent非零cursor当接受；其后反馈或runtime commit仍可能失败。O-004必须对齐Requester journal中的接受前缀与Provider状态恢复条件，不能把decoder本地无状态等价成跨进程原子提交。此项仍OPEN，A7-08未整体关闭。

## Reference Evidence and Required Product Proof

### Production Caller Inventory

本表覆盖当前tracked生产树的`makeNativeStandaloneTokenizerDecoder`、`generationTextDecoder`及factory consumers；CodeGraph查询后以精确路径搜索确认，临时副本不作依据。

| File / symbol | Planned change / owner |
| --- | --- |
| `cpp/ndnsf-di/NativeStandaloneTokenizer.hpp/.cpp` | 新增`NativeGenerationTextDecoders { full, stable }`及`makeNativeStandaloneTokenizerDecoders(options,expectedDigest)`；full签名(ids)→string，stable签名(ids,final)→string，均捕获同一shared_ptr<NativeTokenizer>，skipSpecial固定true，与当前生产helper一致。旧单数factory保留为完整decode兼容API，不能作为stream注入入口。T007 |
| `cpp/ndnsf-di/NativeProviderHandler.hpp` | 新增`GenerationDecodersFactory = function<NativeGenerationTextDecoders(const string&)>`及`generationDecodersFactory`。旧generationTextDecoder/Factory保留非stream兼容；需文本的stream若只有旧factory则配置拒绝，不猜stable语义。T007声明，T011接线 |
| `cpp/ndnsf-di/NativeProviderHandler.cpp::NativeAuthenticatedGenerationConfig` | 保存paired decoders和paired factory；`generationConfigFromAuthenticatedRequest`用sealed.tokenizerDigest调用一次paired factory，替换整个pair，禁止full/stable来自不同摘要或两次独立load；把pair.full/pair.stable传入coordinator。旧unary路径保持。T011 |
| `cpp/ndnsf-di/NativeEpochCoordinator.hpp/.cpp` | 在现有textDecoder旁添加stableTextDecoder；按上文候选/terminal顺序计算delta，requireTextOutput检查pair，重算前缀时抑制事件并重建稳定文本。T011 |
| `examples/DI_NativeProviderExecutable.cpp` | 当前main中唯一生产factory注入点从单数改为paired；保留tokenizerPath捕获及sealed digest来源。T009抽取宿主后对应代码只保留在`NativeInferenceProvider.cpp`公共host内，CLI调用host，不能留下两份factory装配逻辑。T007/T009/T011 |
| `tests/integration-tests/ndnsf-di-core-flow.t.cpp` | 当前三处handlerConfig.generationTextDecoderFactory及单数factory测试：stream场景转paired，保留full-only配置拒绝测试；T011编写，T016运行 |
| `tests/unit-tests/distributed-inference-tokenizer.t.cpp` (planned) | T007注册真实NativeTokenizer ABI/profile/flush/交错调用单测；不使用Python callback替代被测native stable方法 |
| `tests/unit-tests/distributed-inference-stream-recovery.t.cpp` (planned) | T010/T011注册下文accept/replacement/terminal状态单测；与Core传输fixture分开，但测试真实native operation事件处理 |

以上cpp相对路径均位于`NDNSF-DistributedInference/`。factory在认证Selection取得digest后才加载实际tokenizer；准备阶段能力校验不能把调用方随意传入的digest当sealed身份。配对struct只组合回调，不新增tokenizer实例、独立线程或会话状态owner。T012 binding调用公共host/client，T013移除Python运行owner，不新增绑定层factory。

### Requester Acceptance and Recovery Resolution

2026-09-08 R4-B2 operation stride补充：每条普通dependency可含多个tensor transfer，
故streaming_operation_stride为每epoch操作编号的容量，至少为dependency数量且不大于
2**20，不再要求恰等于dependency数量。planner按各dependency的tensor/redistribution
数量预留普通操作，再预留唯一feedback round；group builder核对实际操作编号小于stride，
各epoch以同一stride偏移。旧单tensor/单dependency值仍有效。native parser与过渡SDK
解析器同步此界限，冻结历史wire/证据不回写。feedback不加入单轮readiness。

当前`app_sdk/placement.py::AutomaticStreamingHandle._accept_event`在condition锁内校验attempt/request/generation、连续tokenEpoch和acceptedPrefixDigest，再追加`_token_ids/_events`，锁外调用用户callback。`_begin_replacement`从这份内存前缀创建attempt2；`AutomaticPlanningCoordinator.request_streaming::run_replacement`构造GenerationRecoveryV1并排除失败Provider。此路径**没有逐token runtime journal写入**。`app_sdk/runtime_journal.py::append_many`本身有flush/fsync，但不是当前token接受调用链，不能因此宣称流式前缀跨Requester进程崩溃持久化。

T010的NativeInferenceOperation::State增加/明确`acceptedTokenIds`、`acceptedText`、`acceptedTerminalHint`、`currentAttempt`及`replacementStarted`；由同一serial executor写入，和既有bounded events队列共用operation寿命。`acceptedTerminalHint`初值NONE；不是持久化record。新增private `acceptGenerationEvent(attempt,payload)`、`beginReplacement(attempt,error)`、`validateGenerationFinal(attempt,payload)`处理现有Core callbacks，不另建网络协议。

accept顺序：解析JSON/严格UTF-8及所需字段类型；校验活跃attempt、request/generation身份、连续epoch和token prefix摘要；校验textDelta为字符串及finishHint为NONE/EOS/STOP_SEQUENCE/MAX_TOKENS；检查事件队列容量和token预算；然后原子更新operation接受前缀、文本与terminal hint并投递事件。用户callback抛错导致该operation失败，不能撤回已接受前缀或再次投递同一事件。旧attempt的迟到事件忽略并计数，当前attempt的重复/缺口保留显式lineage failure。网络收到/Provider发布cursor与Requester完成accept是不同边界。

replacement只允许现有eligible错误、attempt1、未启动replacement、剩余原deadline以及`acceptedTerminalHint==NONE`；快照Requester已接受IDs构造原GenerationRecoveryV1，由新Provider从原输入重算并核对。Provider已发布但Requester未接受的token不进入快照；已接受但Provider runtime commit失败的token仍进入快照，由重算恢复，无需假设旧KV已提交。沿用sealed identity/seed/绝对step，不回滚已接受文本，不新增每token分布式commit协议。

如果终止token已接受而最终响应丢失/失败，禁止开启继续生成的replacement，保留接受文本并报告原失败；不能伪造成功finalPayload、续采EOS之后的token或用空白final掩盖。正常final必须核对tokenIds与acceptedTokenIds完全一致、现有wire字段text与acceptedText逐字节一致、终止hint一致；generic非生成stream沿用其原opaque response契约。T010迁移时补齐当前Python只核对token transcript的文本缺口；T012让两入口调用同一native检查。

Requester进程崩溃不保证恢复未完成stream；既有C16 restore只恢复已提交conversation/checkpoint records，temporary/inflight不晋升。该边界延续原能力，不把进程内replacement扩大为新持久化协议。已完成turn的原子journal提交与Provider receipt仍按CD-007执行，stream文本成功本身不等于conversation commit成功。

具名负例：`Spec182StreamRejectBeforeAcceptKeepsPrefix`、`Spec182StreamProviderCommitFailureRecomputesAcceptedPrefix`、`Spec182StreamUnreceivedTokenExcludedFromReplacement`、`Spec182StreamTerminalTokenNoReplacement`、`Spec182StreamFinalTextMismatch`、`Spec182StreamCallbackFailureDoesNotReplay`。T010/T011局部单测，T016真实stream路径；当前NOT_RUN。A7-08算法/调用方/恢复设计现已定义，产品修复证明仍OPEN；O-004其他API/schema/注册寿命清单不因本表关闭。

[ByteLevel reference checker](../../../tests/fixtures/spec182/dependency-probes/check-bytelevel-stream.py)使用独立Python标准库incremental UTF-8 decoder与固定HF ByteLevel完整decode对照：65,536个two-byte序列、9个3/4-byte/截断/非法序列和3个whole-token fallback输入均PASS，exit0。另有ByteFallback 7个反例，未变输入不重跑。本检查验证候选字节算法的参考依据，不执行新增Rust ABI或native产品，不能写为T007/PO-006 PASS。

T007必须在真实C ABI/C++层覆盖同一输入、special两个选项、未知/负ID、大小限额、final flush、合法U+FFFD和同owner多个交错调用。T011覆盖event接受前拒绝、接受后失败、EOS/MAX/stop、恢复抑制重复事件，断言事件delta拼接等于finalPayload文本；T016完成既定真实请求与no-Python验证。当前均NOT_RUN。
