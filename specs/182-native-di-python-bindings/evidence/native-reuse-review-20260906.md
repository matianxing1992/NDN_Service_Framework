# Native Capability Reuse Review

**Scope**: Spec182 revision 7 / native-library reuse and unnecessary custom implementation
**Source**: Experimental `5239b2296aee5dc4fb6a4f2599e5ceed94ea504e`
**Verdict**: BLOCK for implementation; native ownership direction retained

## Conclusion

原生化目标合理：应用直接链接DI库，Python可选；“原生”允许复用Rust/C/C++依赖，不要求把成熟库重写成C++。当前设计没有要求重写ONNX执行引擎、分词算法或再造NDN协议。但不能确认“所有环节已有可直接替换的原生API且设计无缺陷”：发现流式解码接口缺口和已有C++采样与Python参考的两处不一致。它们应在T001的O-004迁移清单中明确处置，不能因旧原生代码存在就自动当作正确基线。

本轮是审计和reference诊断，没有修改产品源码、替换依赖、构建native产品或运行integration/MiniNDN/SIF/Tiger。O-002/O-004保持OPEN，T001及17项产品任务均未完成。已记录的ONNX identity缺陷A7-07仍控制其原范围。

## Reuse Map

| Concern | Existing native support / current choice | Assessment |
| --- | --- | --- |
| ONNX inference / CUDA | 现有`cpp/adapters/onnx/OnnxRuntimeModelRunner.cpp:300-332`直接使用`Ort::Env`、`Ort::SessionOptions`和CUDA EP | 已复用官方引擎；保留runner，不重新实现算子/CUDA推理 |
| ONNX parse/check/shape/serialize | `native-dependency-design.md:12-39`选官方ONNX1.17.0、protobuf；四个冻结装配向量原生bytes一致 | 正确复用。自有部分是认证recipe与NDNSF身份、graph extraction组合逻辑；官方1.17的`onnx.utils.Extractor`本身是Python实现，不能把ORT执行API误当成等价装配API |
| Assembly worker | `native-onnx-assembly-design.md:13,70-78`明确独立原生进程 | 有具体理由：保留不可取消checker/shape调用的超时终止与回收。仅冷装配使用，不是每token服务或新网络协议；不应为追求无子进程而删除 |
| Tokenizer | 同版本HF tokenizers0.20.3的Rust核心，五函数C ABI与C++ RAII；special flags及84完整文本向量保持 | 这是native库复用，不是自造BPE。现有理由支持小型bridge；不能因有C++外壳就声称所有依赖都由C++编写 |
| Generation / state | CD-007复用`NativeEpochCoordinator`、Provider state；T011明确禁止新生成运行时 | 分布式epoch、grant、lineage、commit、远端角色状态仍归NDNSF；模型库的KV管理不自动替代它们。已有采样正确性另见A7-09 |
| Stream decoding | 当前CD-006只有stateless encode/decode；官方GenAI提供`OgaTokenizerStream` | 当前方案存在A7-08；完整decode可用不代表逐token前缀稳定 |
| Chat template / multimodal / MTP | 官方GenAI具备多项相关接口，但当前182只承接已有YOLO/Qwen能力 | 不把粘贴材料中的Qwen3.8、视频、tool calling、MTP升级为182既有能力或新增必做项；本轮未验证这些模型/导出包兼容性 |
| NDN orchestration / authorization | FR-002复用Core Begin/Commit与Provider验证；原生adapter承担模型差异 | 属于项目自身职责，不能靠调用`GenerateNextToken`省去，也不能重造第二套协议 |

上述官方能力依据：[ORT C/C++ API](https://onnxruntime.ai/docs/api/c/)、[GenAI C++ API](https://onnxruntime.ai/docs/genai/api/cpp.html)、[ONNX v1.17 Extractor source](https://github.com/onnx/onnx/blob/v1.17.0/onnx/utils.py)。官方接口存在不证明它兼容当前认证图、tokenizer选项、随机序列和分布式状态；本表是源码与接口审查，不是GenAI接入测试。

## Findings

### A7-08 HIGH / OPEN — Streaming decode contract

`NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.cpp:977-992`每个token对完整IDs调用textDecoder，再要求candidateText以已提交generatedText开头，否则抛`native tokenizer rewrote committed text prefix`。当前`NativeStandaloneTokenizer.cpp:56-105`启动Python helper；计划CD-006将其换为同语义的原生完整decode，没有定义pending bytes、稳定文本片段、finish/flush或恢复状态。PO-006目前只有完整ids/UTF-8向量。

用现有、未修改的`byte-fallback-special` fixture和tokenizers0.20.3实际诊断：

| Text | IDs | Prefix decodes | First non-prefix step |
| --- | --- | --- | --- |
| 你 | 4 | 你 | none; 单token不是有效反例 |
| 好 | 234,170,194 | `�` → `��` → `好` | 3 |
| 🙂 | 245,164,158,135 | `�` → `��` → `���` → `🙂` | 4 |
| é | 200,174 | `�` → `é` | 2 |
| 中 | 233,189,178 | `�` → `��` → `中` | 3 |

因此旧前缀检查会拒绝这些合法token序列；本轮实际运行的是reference decoder，native拒绝由上述源代码推导，未声称真实模型采样到了这些序列。HF固定版本[ByteFallback源码及测试](https://github.com/huggingface/tokenizers/blob/v0.20.3/tokenizers/src/decoders/byte_fallback.rs)也明确体现不完整字节的替换行为。

**Action / owner**: T001/O-004明确stream decoder契约及兼容边界，优先评估库已有stream API，必要适配仅管理稳定输出与pending状态，不自行发明BPE。T007实现，涉及epoch提交/停止的最小接线由T011收口；T016验证逐token事件拼接、最终完整文本、EOS/stop/cancel/restore。不能通过删除prefix校验或吞掉replacement字符假装修复。84个完整文本对照仍有效，但不关闭这项stream义务；O-003已通过的ABI探针不被追溯改成失败。

### A7-09 HIGH / OPEN — Existing native sampler differs from reference

`NativeEpochCoordinator.cpp:550-559,604-619`与`adapters/qwen/generation.py:51-60,95-109`的数学行为不相同：

1. C++ Top-P确定retained之后，仍以截断前的total计算draw，并把剩余概率落到最后一个retained token；Python使用sum(retained_weights)。对概率`[0.6,0.3,0.1]`、top_k=3、top_p=0.8、temperature=1、seed=8、step=0，u=`0.6185046250316943`，Python实际返回0，当前C++源逻辑会返回1。条件分布应在保留集合归一化；此例当前native等效分布为`[0.6,0.4]`，参考为`[2/3,1/3]`。
2. Python对generated去重后施加repetition_penalty，C++按出现次数重复施加。logits=`[4,1.5]`、generated=`[0,0]`、penalty=2、Greedy：Python实际调整为`[2,1.5]`、返回0；C++源逻辑调整为`[1,1.5]`、返回1。

以上是当前源码差异加独立Python reference执行，没有构建/执行native sampler。第三方对照：[GenAI CPU sampler](https://github.com/microsoft/onnxruntime-genai/blob/main/src/search.cpp)复用采样分布并对保留概率使用categorical抽样；不是建议直接依赖其内部类或无版本main。

**Action / owner**: T001/O-004登记既有C++/Python能力差异，明确期望语义及旧seed/oracle兼容处置；T011进行必要局部修复或经论证的库适配，禁止重写整个生成runtime。加入上述两个具名单测，再由T016验证完整token/text。不能以“两个入口都调用同一个错误native实现”替代独立oracle。

### A7-10 MEDIUM / OPEN — Native reuse comparison incomplete

`native-dependency-design.md:51-61`比较了HF Rust与mlc tokenizers-cpp，没有比较GenAI的tokenizer/stream能力。应补一张小型选型表，检查standalone工件加载、add/skip special flags、错误ID/UTF-8、stream状态、依赖体积和模型包要求。官方C++文档中的Encode/Decode签名未提供当前两个布尔选项；这使现有bridge具有合理性，但不是证明所有GenAI版本都无法实现兼容。

GenAI提供生成、采样、KV及stream能力；现有NDNSF负责NDN角色调度与受保护状态。采用整套GenAI可能引入第二个生成/状态owner，因此不应仅凭API列表立即换引擎。T001补“直接调用 / 最小适配 / 项目职责”决策，优先解决A7-08/09；不追加全模型重导出或新benchmark来决定是否需要一张选型表。

### A7-11 LOW / OPEN — Stale authorization sentence

`plan.md:57`仍写“当前授权仅文档”，与同文件Current Planning Result和tasks当前T001授权相矛盾。后续整理为历史说明或删除过时句，避免恢复任务时误停。它不改变本次用户仅要求审计的范围，本次未顺带修订设计。

## Evidence and Checks

- Spec Kit prerequisites、strict structure、design validator及`git diff --check`本轮PASS：19 FR、11 SC、17 tasks、0 complete，163个本地链接。这些只证明审计记录的文档完整性，不是产品PASS。
- 任务按库/策略/sealer/grant/格式适配/host/requester/会话/binding/迁移/验证组织，各有不同闭合行为；未发现需要因本次审计把17项机械拆成更多任务的理由。发现挂回既有T001/T007/T011/T016，不新增平行实施阶段。
- Context Mode project健康PASS；初次active检查因spec/plan/tasks source hash过期失败，采用当前仓库。project检索先两次被identifier guard拒绝，改用精确项目绝对路径后通过并返回正确anchor。未清空或把旧索引当checkpoint。
- CodeGraph已调用，但宽查询混入临时Rust/旧副本；之后按生产绝对所属路径读取并核对，不把临时目录结果用作实现证据。
- 本轮无native依赖重建、产品unit/integration/MiniNDN/SIF/Tiger结果。此前ONNX四向量、tokenizer84+14结果仅按原范围复用。
- 诊断命令：`/usr/bin/timeout 15s /usr/bin/python3 .codex-tmp/spec182-native-reuse-review-20260906-r1/diagnostic.py`，exit0。原始脚本与`result.json`保存在同一raw目录；本记录包含可移植输入、版本、结果与源位置。首次单独检查“你”没有触发问题，随后检查多byte-token字符确认边界，不隐去该阴性观察。
- 首次本地commit被pre-commit全索引引用检查拒绝，exit1；raw `commit-r1.json`记录首边界。hook明确提供`NDNSF_LOCAL_CHECKPOINT=1`用于本地checkpoint引用，保留禁止路径检查；采用该模式，不修改hook或清理历史文档。其他owner同时追加的typed-complex failure记录用index局部patch隔离，不纳入本审计提交。

| Observed source / artifact | SHA256 |
| --- | --- |
| NativeEpochCoordinator.cpp | `f074d6b3d90f186198113a4fdb279e865a5aeeb44c52eed54b16832d7db6b2b6` |
| NativeStandaloneTokenizer.cpp | `71282f18bdb771246dab07c125380f615093951b2d735fe4dc4400e3089cca23` |
| adapters/onnx/OnnxRuntimeModelRunner.cpp | `2198981fccae1537ef333ce7a5dff6da671d2c4cce9a860aca8f72bb364b7ffe` |
| adapters/qwen/generation.py | `3affb6f438a4134bb8e69222d79b3f2ec5a6b256bf0022af636b065627397c11` |
| tokenizer/vectors.json | `6f44a2e62bebccd4dee2ff214d4d65f99528899e6863705eb1ec431b098ed34b` |
| embedded byte-fallback-special tokenizer | `3aef42a9cf6eb91f711bd2fbc3c885e29a9b8b44945c5ab050216369f6629a5f` |

## Next Action

继续T001，补原生复用决策、stream decoder及采样差异处置；同时保留O-002 identity与O-004注册/兼容清单的原关闭条件。设计关闭后按既有任务做最小修复，避免将语言迁移扩大为重建模型引擎。完成记录的文档检查不改变BLOCK或任何产品任务勾选。
