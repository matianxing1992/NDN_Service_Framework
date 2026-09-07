# Native Generation Reuse and Sampling Contract

**Status**: PARTIAL / T001 IN_PROGRESS / product NOT_STARTED
**Owners**: CD-006, CD-007 / T007, T011, T016
**Baseline**: [native reuse review](../evidence/native-reuse-review-20260906.md)

## Reuse Decision

本契约关闭A7-10选型比较，冻结A7-09的采样修复语义；A7-08流式状态设计和O-004完整迁移清单继续OPEN。不以库API存在推断当前模型包兼容，也不以设计决策代替产品验证。

| Concern | Direct reuse | Adaptation / decision |
| --- | --- | --- |
| Tensor execution | 现有ORT Session、CUDA EP | 保留OnnxRuntimeModelRunner的tensor/role接口，不另建算子或GPU引擎 |
| Standalone tokenizer | 固定HF tokenizers0.20.3 Rust核心 | 保留五函数C ABI的owned bytes、错误、RAII及special flags；84个完整文本向量不证明stream |
| GenAI tokenizer | 官方OgaTokenizer以OgaModel创建；Encode/Decode示例没有add/skip special布尔参数 | 当前工件为独立tokenizer.json；替换须另证配置、special行为、错误ID/UTF-8和释放。保持现有HF核心，不宣称GenAI所有版本都不支持这些能力 |
| GenAI stream | 官方OgaTokenizerStream提供逐token Decode | 还需对齐候选/提交/回滚、EOS flush、stop、恢复；作为原生能力对照，不据API示例宣称A7-08关闭 |
| Sampling | 现有NativeEpochCoordinator及SplitMix64 | 局部修复数学差异；固定seed兼容要求不能被不同RNG的categorical调用静默替换，不依赖GenAI内部Search ABI |
| KV / generation state | 既有Provider runtime、epoch/state | NDNSF拥有受保护角色状态、grant、lineage和网络commit；不增加OgaGenerator作为第二个状态owner |

依据为2026-09-07读取的[官方GenAI C++接口](https://onnxruntime.ai/docs/genai/api/cpp.html)及[HF 0.20.3源码](https://github.com/huggingface/tokenizers/blob/v0.20.3/tokenizers/src/tokenizer/mod.rs)。GenAI文档标记preview，本项目未锁定/安装GenAI产品依赖。HF固定文件提供完整decode，本轮未发现DecodeStream；不据此否定其他文件/版本。GenAI依赖体积、构建时间、内存和模型包接入成本均未测量；保留现有方案依据是已固定工件与语义闭包，不是未经测量的性能判断。

## Sampling Source Changes

修改`NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.cpp`现有私有`sampleToken(outputs, config, generated, step)`，复用`lastLogits`、`splitmix64`、`deterministicUnit`。不新增public类、配置字段、协议、RNG服务或持久化字段。T011实现，绑定及应用入口共享此函数。

| Parameter / value | Meaning and required behavior |
| --- | --- |
| outputs | 已完成角色的TensorBundle；lastLogits取最后时间步float32并拒绝非有限值，来源和tensor契约不变 |
| config | 现有sealed mode/temperature/top_k/top_p/penalty/seed；两个mode计算前统一校验 |
| generated | 之前接受的token IDs；每个合法ID只惩罚一次，负数/越界沿用参考忽略，不额外加入prompt |
| step | 现有epoch传值；恢复沿用绝对位置，不重置，也不改变event编号转换 |
| adjusted logits | float32精确提升double后施加惩罚；避免旧C++转回float与Python float精度差异 |
| retained weights | Top-P截断后集合；draw使用其总和，不能用截断前total |

统一校验：mode为Greedy或SeededTopKTopP；vocabulary非空；`1 <= top_k <= vocabulary`，不静默clamp；top_p和penalty有限，范围分别(0,1]、[0.1,2]。Greedy temperature=0；随机模式temperature有限且在(0,5]。native seed为uint64；binding转换前拒绝负数/溢出。非法输入沿用`std::invalid_argument`及绑定错误边界。native非有限logits拒绝保留；Python Greedy旧函数接受部分非有限输入不是正确兼容目标。

算法：float32→double；对generated去重ID，非负logit除penalty、负logit乘penalty；Greedy并列取最低ID。随机模式按logit降序、ID升序取top_k，除temperature并减最大值后exp；按该集合total累计概率，保留第一次达到top_p的最短前缀；`u = (SplitMix64(seed + step mod 2^64) >> 11) / 2^53`；`draw = u * sum(retained_weights)`；顺序累计并严格比较`draw < prefix`，最后元素仅作舍入兜底。保持现有随机算法和生成runtime。

## Compatibility and Proof

采用Python参考对有效输入的数学语义，不保留错误native分布模式。受影响seed/token/text oracle保存旧结果及变化原因，再用独立参考验收；历史PASS不改写。复用schema与sampling digest不保证错误/修复版本输出一致：交付固定commit，进行中会话不切换二进制。既有committed prefix不可改写；恢复重算不符保留prefix mismatch拒绝和原journal，不绕过校验。完整跨版本journal迁移仍由O-004关闭。

T011新增选择器`Spec182SamplingTopPRetainedMass`、`Spec182SamplingPenaltyOncePerToken`、`Spec182SamplingValidationParity`、`Spec182SamplingDoublePrecisionAndTies`，经真实epoch调用私有sampleToken；禁止复制native算法作为被测对象。具名测试文件及注册由T011完整清单收口。已运行Python参考的两个输入见review：概率[0.6,0.3,0.1]对应logits、top_k=3、top_p=0.8、temperature=1、seed=8、step=0应选0；[4,1.5]、generated=[0,0]、penalty=2、Greedy应选0。真实tensor测试的输入先量化float32，再送Python参考，不拿float64源值代替传输值。

另覆盖正负logit重复ID、并列、top_p=1、top_k=1、seed/step回绕、边界/非有限参数。不同libm的任意临界浮点输入不保证逐位相同；交付固定工具链，并验证阈值两侧向量。T016继续完整token/text、续接与恢复证明。当前native单测和最终运行均NOT_RUN。

## Streaming Closure Required

### Verified Boundary and Planned ByteFallback Algorithm

2026-09-07新增[独立参考检查](../../../tests/fixtures/spec182/dependency-probes/check-stream-boundaries.py)，固定既有tokenizer JSON SHA及tokenizers0.20.3。`<0x61>`解出`a`，追加`<0xFF>`或未完成的`<0xE5>`后，完整decode变成两个U+FFFD；即使当前文本没有replacement，也不代表可提交。合法U+FFFD的三个byte token最终必须保留一个U+FFFD，不能用删除该字符作为修复。普通非byte token结束byte run；skipSpecialTokens=true时被过滤的special token不结束run，false时保留的非byte special才结束run。

固定版本[官方ByteFallback实现](https://github.com/huggingface/tokenizers/blob/v0.20.3/tokenizers/src/decoders/byte_fallback.rs)以整个连续byte run调用String::from_utf8，失败则每byte返回一个replacement。由此选择的适配算法是：对过滤special后的token序列，暂存最后尚未由非byte token结束的整个byte run；只将此前闭合部分交给原HF decoder作为稳定文本；正常终止时再用完整decode flush尾run。不能仅保留最后最多3个UTF-8字节，因为后续无效byte会改变此前整段。run长度受既有生成token上限约束，不添加静默截断。极端全byte输出可直到终止才产生text delta，但每个接受token仍按原协议发布token事件；这不是把所有decoder改成结束后一次输出。

当前三份fixture的decoder分别为null、WordPiece(prefix为空且cleanup=false)、单独ByteFallback。前两种按当前完整decode可逐步形成稳定前缀；上面的run算法仅定义单独ByteFallback，不能未经证明套用Sequence/Replace/Strip/ByteLevel或真实Qwen tokenizer。完整encode/decode能力保持，生产stream能力必须对实际工件的decoder pipeline单独核对；不可为通过fixture而缩减真实模型范围。

HF0.21.0提供[DecodeStream/step_decode_stream](https://github.com/huggingface/tokenizers/blob/v0.21.0/tokenizers/src/tokenizer/mod.rs)，维护ids/prefix/read_index/prefix_index并对末尾U+FFFD暂缓输出。本轮源码比较说明“HF没有原生stream能力”不能作为跨版本结论；但其算法不是已证明满足上述完整ByteFallback finalText、合法U+FFFD flush及NDNSF事务边界的替换。暂不升级已锁定0.20.3或复制新版实现；旧84个完整向量结果保持原范围。

候选状态仍必须与提交状态隔离：preview接收candidate IDs及是否终止，返回owned稳定文本，拒绝事件或取消时丢弃候选；终止判定应先根据完整decode判断既有stop suffix/EOS/MAX_TOKENS，再flush当前候选，不能先发布不稳定文本后用finalPayload补救。既有eventSink接受后仍可能发生反馈发布或runtime commit失败；已发布事件无法撤回，恢复需要journal/event接受边界的显式协议，不能仅靠本地decoder rollback宣称事务回滚。该跨层问题继续由O-004和T011关闭。

A7-08继续阻塞相关接线：需明确pending bytes与稳定片段算法、preview/commit隔离、EOS/MAX_TOKENS flush、stop、cancel/事件拒绝和恢复状态，以及调用方/字段。全量decode后删除U+FFFD或任意缓冲几个token不是通用正确方案；合法U+FFFD、decoder cleanup、special token和byte fallback分别覆盖。五函数ABI目前只承诺完整encode/decode，不改写为stream-ready。
