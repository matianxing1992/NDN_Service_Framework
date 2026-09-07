# Native Token Stream Design

**Status**: ALGORITHM_DEFINED / T001 IN_PROGRESS / implementation NOT_STARTED
**Scope**: CD-006, CD-007 / A7-08 / T007, T011, T016

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

[ByteLevel reference checker](../../../tests/fixtures/spec182/dependency-probes/check-bytelevel-stream.py)使用独立Python标准库incremental UTF-8 decoder与固定HF ByteLevel完整decode对照：65,536个two-byte序列、9个3/4-byte/截断/非法序列和3个whole-token fallback输入均PASS，exit0。另有ByteFallback 7个反例，未变输入不重跑。本检查验证候选字节算法的参考依据，不执行新增Rust ABI或native产品，不能写为T007/PO-006 PASS。

T007必须在真实C ABI/C++层覆盖同一输入、special两个选项、未知/负ID、大小限额、final flush、合法U+FFFD和同owner多个交错调用。T011覆盖event接受前拒绝、接受后失败、EOS/MAX/stop、恢复抑制重复事件，断言事件delta拼接等于finalPayload文本；T016完成既定真实请求与no-Python验证。当前均NOT_RUN。
