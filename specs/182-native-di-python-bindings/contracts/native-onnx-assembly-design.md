# Native ONNX Assembly Design

**Status**: DESIGN_FROZEN / T001 / O-002 CLOSED; T006 implementation NOT_STARTED
**Authority**: [CD-005](code-design.md#cd-005-assembly), [dependencies](native-dependency-design.md), [proof](proof-design.md)
**Source baseline**: `11e8d750` / Experimental; production sources unchanged from `81e251a4`

## Source Findings and Decisions

当前Provider的`prepareNativeCanonicalOnnxRole`已经负责真实assignment root、两个加密大对象的name/size/digest、ProtectedRuntime与scratch lease、签名、密文缓存和最终runner spec。只替换其格式操作，不把这些职责下移到adapter。`NativeSelectionProjectionV3::assembly`已经是`NativeSelectionRoleV3`；不新增第二份可变recipe字段表。

Python `assemble_certified_onnx_model`先验证recipe/role、source/external对象、full checker和canonical identity，再执行Extractor、逐node wire比较、I/O与最终ORT CPU加载。`canonical_onnx_identity`对未shape-infer的原图计算身份，Extractor则在独立副本上infer_shapes。混用这两个model会改变graph digest。原helper最终生成`ndnsf-di-assembled-onnx-v1`manifest，Provider签名并激活；两者不能合并为“ONNX parse成功”。

现有`runPythonHelper`可在deadline/cancel时终止子进程。ONNX1.17.0 checker/shape inference的接口没有取消参数，直接搬进Provider线程无法保留有界清理。决定：格式算法仍属于同一原生DI库，公开assembly调用经一个随库安装的**原生装配worker**运行不可中断的第三方操作。worker不含Python，不是网络服务，不接收凭证/密钥，不拥有Provider缓存或授权状态。它是公开交付清单中的runtime executable，不能伪装成“无子进程”。

## Planned Paths and Types

| Operation / path | Owner / exact change |
| --- | --- |
| ADD `NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp` | C14公开adapter入口及下面三个owned值类型；不公开ONNX/protobuf类，不改变Core wire |
| ADD `NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.cpp` | C14私有identity/extraction/checker/ORT算法和manifest数据构造；worker与测试使用同一实现 |
| ADD `NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp` | DI私有worker协议声明、`runNativeOnnxAssemblyWorker`与`runNativeOnnxAssemblyWorkerMain`；不安装为应用public header |
| ADD `NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.cpp` | 父进程FD通信/超时/回收与子进程协议入口；没有Face、网络authority或缓存 |
| ADD `examples/DI_NativeOnnxAssemblyWorker.cpp` | `main(argc,argv)`只调用私有worker main，固定`--stdio-v1 --metadata-bytes <length>`模式；length由父端实际已序列化metadata得出，不能接受任意source/output路径或shell命令 |
| MODIFY `examples/wscript`, root `wscript` | T002声明/T006接线`di-native-onnx-assembly-worker`目标，安装到`${LIBEXECDIR}/ndnsf-di/`，链接同一个DI库；安装prefix和runtime dependency identity可迁移 |
| MODIFY `NativeCanonicalOnnxAssembler.hpp/.cpp` under `cpp/ndnsf-di/` | 保留两个prepare重载及保护/缓存流程，删除Python参数、OwnedAssemblyHelper和旧request/result文件IPC；增加`assemblyTimeoutMs=30000`替代helper私有时间上限。`shouldCancel/signManifest/protectedRuntime/roleAssemblySpecDigest/cacheDir/providerIdentity`含义不变 |

以下类型均在`ndnsf::di`，构造后由本次assembly调用拥有，不能跨请求缓存明文：

| Type / field | Exact type / initial value | Meaning / owner |
| --- | --- | --- |
| `NativeCanonicalSource::modelBytes` | `vector<uint8_t>` / empty | Provider验证对象name/size/digest后移动进来的ONNX字节；调用期间借用，退出零化 |
| `NativeCanonicalSource::initializerBytes` | `optional<vector<uint8_t>>` / nullopt | 已独立认证的external对象；nullopt与存在但空字节不同，不能用空vector代替缺失状态 |
| `NativeCertifiedRecipe` | `using NativeCertifiedRecipe = NativeSelectionRoleV3` | 复用已绑定projection的字段；input/output names按现有native bridge从expected contracts取name并排序，expected contracts自身顺序保留 |
| `NativeAssemblyControl::deadline` | `steady_clock::time_point` / 显式必填 | 父进程创建时取协议剩余时间与assemblyTimeoutMs较小值换算一次；wall-clock回拨不延长 |
| `NativeAssemblyControl::requireActive` | `function<void()>` / 显式必填 | 父进程注入的原生权限/取消检查：委托现有requireActiveAssembly及ProtectedRuntime；抛异常终止。不是Python callback，不传给worker，不替代外层独立授权 |
| `NativeCertifiedAssembly::modelBytes` | `vector<uint8_t>` / empty | 完整INLINE_ONNX结果；Provider重新算hash和限额后消费，RAII零化 |
| `NativeCertifiedAssembly::inputNames/outputNames` | `vector<string>` / empty | 抽取结果实际有序I/O；与认证recipe对照后返回 |
| `NativeCertifiedAssembly::nodeCount` | `uint64_t` / 0 | 实际抽取node数，必须等于认证nodeIndices长度 |
| `NativeCertifiedAssembly::modelDigest` | `string` / empty | `sha256:`+完整输出字节摘要；父进程自行复算，不能只信worker文字 |

所有model/initializer/结果/IPC临时byte buffer持有`NativePlaintextBufferGuard`或等价同owner RAII；不得留下跨请求全局缓存。源和返回值move语义，禁止为IPC生成完整hex/base64导致多倍内存；ONNX protobuf内部临时对象与worker一同回收。

## Functions and Call Edges

| ID / signature | Parameters / result / precise work |
| --- | --- |
| OA01 `assembleNativeCertifiedOnnxModel(const NativeCanonicalSource&, const NativeCertifiedRecipe&, const NativeAssemblyControl&) -> NativeCertifiedAssembly` | public格式入口；校验非空control及源/recipe限额后调用OA02；只返回有界worker的完整结果，不直接在Provider做不可取消的checker |
| OA02 `runNativeOnnxAssemblyWorker(source,recipe,control) -> NativeCertifiedAssembly` | 私有transport；固定安装worker和已登记库身份，启动、双向有界FD通信、轮询cancel/deadline、验证结果、waitpid。任何晚到成功均须再次requireActive，不能在取消后发布结果 |
| OA03 `runNativeOnnxAssemblyWorkerMain(int argc,char** argv) -> int` | 只接受固定模式和完整十进制metadata length，先关闭除stdio外全部FD；完整解析一个request frame、校验limits/字段后调用OA04；只输出一个response frame，stderr沿用64KiB限额且不含源/secret。0成功，1算法拒绝，2协议输入错误 |
| OA04 `assembleInProcess(source,recipe) -> NativeCertifiedAssembly` | worker私有函数；顺序执行下面S1--S8；不启动第二个worker、不调Core/Repo、不写cache或签名manifest |
| OA05 `parseAndInlineSource(source,recipe) -> onnx::ModelProto` | 验证protobuf、external位置/offset/length与对象存在性；owned model，所有external读取都来自传入字节，不打开模型内声明的路径 |
| OA06 `canonicalIdentity(const onnx::ModelProto&) -> NativeOnnxIdentity` | 对尚未shape-infer的model计算graph和normalized initializer两摘要；内部记录见下面identity规则。泛化的tensorMap/parameterConfig/semantics输出由O-004 graph adapter清单统一归属，不能让本helper冒充全部graph inspect实现 |
| OA07 `extractCertifiedGraph(const onnx::ModelProto&,const NativeCertifiedRecipe&) -> onnx::ModelProto` | 在副本infer_shapes，按认证I/O边界逆向搜集node并按原序复制，保留引用tensor和local function；重建1.17.0 Extractor metadata，独立验证node cover与I/O |
| OA08 `deterministicModelWire(const onnx::ModelProto&,uint64_t maxBytes) -> vector<uint8_t>` | 先ByteSizeLong与限额/size_t/protobuf API边界检查，再CodedOutputStream deterministic序列化；流析构/错误确认后才返回bytes |
| OA09 `makeNativeAssemblyManifest(const NativeCertifiedAssembly&,const NativeSelectionProjectionV3&,const string& modelName,const string& modelDigest,const string& provider) -> string` | 父Provider格式模块构造下面既有manifest canonical JSON；传入已认证root的model identity和实际Provider identity，输出待签名字节，不自己签名或声明业务完成 |

`NativeOnnxIdentity`是私有值`{string graphDigest; string initializerDigest;}`，无default可信状态。OA06的临时`tensorIndex`按tensorName排序；`contentToNames`只用于共享内容别名；graph/node/value facts、shape-inferred副本、reachable node集合仅活到本次worker退出。OA02的`pid`、owned process group、三条pipe FD、read/write offsets、expected frame length、steady deadline、terminal flag由单一父线程拥有；非阻塞poll处理中不会让业务Face线程执行等待。

## Algorithm Contract

1. **S1 Recipe binding**：保留当前RoleAssemblySpec/CertifiedOnnxAssemblyRecipe的canonical SHA256格式、schema、非空backend/role kind、COMPONENT_SET的0/0 interval、range非空interval、严格递增无重复nodeIndices、精确expected I/O name集合与所有digest/backend/precision/quantization/layout/padding/resource字段绑定。Provider在OA01前仍使用已认证projection；worker重查格式及recipeDigest，不把接收帧当授权证据。canonical recipe JSON使用原camelCase key、排序key、UTF-8、compact separators；不按snake_case IPC别名计算digest。复用当前jsonContracts的canonical integer dimension规则，数字字符串语义比较不能修改已绑定recipe digest。
2. **S2 Source and external data**：graph和可选initializer各自非空/size不超maxSourceBytes，累计分配检查加法溢出。外部tensor每个恰好一个location，禁止absolute/`..`/空basename；所有location必须相同。可有安全相对子目录名，不能错误收窄为只有basename。只在内存归一化到`model.onnx.data`，offset/length为有界非负整数、范围不溢出；缺length或length=0均从offset取剩余（1.17 loader的实际行为）。有external必须有initializer，无external禁止附带initializer。覆盖1.17 `_get_all_tensors`遍历的graph及其嵌套graph initializer、node attribute tensor和function attribute tensor；在任何拷贝前对全部external字段施加同一位置/范围检查，不允许实际打开模型声明路径。内联后设置DEFAULT、清除external_data，避免function遍历重复读取。
3. **S3 Validate before identity**：对内联后的原model执行ONNX1.17 checker full_check，验证节点上限、layerEnd/nodeIndices不越界。禁止unknown protobuf丢失后仍声称原字节identity等价。checker shape推断不写回OA06的原model。
4. **S4 Canonical identity**：initializer按name排序，拒绝无稳定name；按[initializer normalization](initializer-normalization.md)取得dtype/shape/content并检查需要v2的assemblerDescriptorDigest，普通稳定numeric保留旧结果，已证实错误的表示不得走旧路径。每项字段为tensorName/dtype/shape/byteOrder/contentDigest/byteLength；相同contentDigest的名字排序，多于一个时sharedReference取第一个，否则空字符串。graph facts包含irVersion、按domain/version排序的opsets、原序nodes（index/domain/opType/inputs/outputs及按attribute.name排序的deterministic proto hex）、原序input/output proto hex、排序valueInfo proto hex、去掉contentDigest的initializerLayout、排序function proto hex。对compact sorted-key UTF-8 JSON算SHA256。initializerDigest只对有序`{tensorName,contentDigest}`数组算hash。文件名、external打包布局不进入身份。两摘要都必须等于recipe才进入S5。
5. **S5 Extract**：副本用官方InferShapes默认选项；输出逆向DFS遇到recipe input即停止。用显式栈/visited避免C++递归栈溢出，按原node索引输出。I/O按当前recipe排序的inputNames/outputNames重建；原graph已有I/O优先，其余从推断value_info取；找不到拒绝。保留reachable node所引用initializer/value_info的原序；原Extractor拒绝的sparse_initializer/quantization_annotation继续拒绝。local function按第一次引用顺序搜集，递归搜集函数node引用的函数并去重；不得像四向量探针那样直接拒绝全部local function。结果graph.name为`Extracted from {<original name>}`，保留ir/opset、producer_name=`onnx.utils.extract_model`、上述functions；其他meta只按1.17 make_model/make_graph行为设置，不复制全部源metadata。
6. **S6 Exact cover and I/O**：full checker后实际nodeCount严格等于nodeIndices.size，实际node的deterministic proto bytes逐项等于原model指定node；不是仅比较名字/opType。tensor dtype数字别名按旧_ONNX_DTYPE_NAMES规范化；shape具体int与可解析的十进制文本按旧normalize_shape_dimension语义比较，符号字符串保持原值；rank/name/dtype/每个维度均相同才通过。
7. **S7 Serialize and load**：确定性序列化，非空且≤maxAssembledBytes；用已锁定ORT实际CPU InferenceSession/Session从这些字节加载，析构后才返回。此处只证明模型可加载，不把CPU预检当Provider实际CUDA执行。实际device/backend和protected activation仍由原NativeProviderRuntime/runner验证。
8. **S8 Parent validation and activation**：父进程验证response长度/版本/schema、结果SHA256、node/I/O，再requireActive并构造manifest/签名；保持现有ProtectedRuntime withContentKey、seal/openNativeAssembledEntry、lease、冲突检查、runner metadata与terminal outputScope。取消/过期/权限变化后不写新明文、不签名/激活；任何算法失败只清理本请求。未受保护内容地址缓存及受保护ciphertext cache原有差异保留。

## Native Worker Framing and Lifetime

协议只用于本机匿名pipe，**不是NDN wire format**。request header：8 bytes ASCII `NDI182A1`、uint64 little-endian metadataLength、uint64 modelLength、uint64 initializerLength、uint8 hasInitializer。metadata为UTF-8 canonical role/recipe JSON，随后原始model/initializer bytes。父端先验证已认证recipe的结构/资源预算，metadataLength取实际序列化字节数并通过固定argv传递；child要求frame的metadataLength与该值一致且可由size_t表示，再分配读取。不新增会截断既有大node cover的128KiB请求限额。hasInitializer仅0/1且与长度/存在状态相容；所有声明长度先验证再分配，尾随额外字节/第二帧拒绝。

response header：8 bytes `NDI182R1`、uint32 metadataLength、uint64 modelLength、uint8 status（0成功/1算法拒绝/2输入错误）；metadata沿用原MaxHelperMetadataBytes=65536上限，成功含inputNames/outputNames/nodeCount/modelDigest，失败只有固定boundary/code和有界脱敏message；失败modelLength必须0。结果bytes≤recipe.maxAssembledBytes。父端不信child宣称checker PASS而跳过自身长度/hash/权限验证。

父进程用posix_spawn固定argv和最小env，独立process group，仅映射stdin/stdout/stderr；其余已知FD CLOEXEC/close。child在进入任何业务操作前通过自身/proc/self/fd再关闭全部非stdio FD，避免多线程父进程枚举FD期间新建描述符的泄漏；不依赖本机glibc未提供的closefrom spawn扩展。worker不在fork后的Provider地址空间执行算法，而在exec后的干净程序中工作。pipe设nonblocking，poll同时推进写source与读result/stderr，防止大输入/输出互等死锁；每轮及终态检查steady deadline和requireActive。取消/超时先TERM，最多1秒后KILL并waitpid；先封闭返回/激活门，再回收worker。partial frame、child signal/非零退出、EOF/extra frame、未退出worker均不返回成功。正常结果也要确认child exit0；stdout只用于协议，日志走有界stderr。

worker只有已验证model/initializer明文字节和公开recipe，不接收签名器、credential、content key、ProtectedRuntime或artifact网络名称。安装库的固定worker路径/二进制身份必须进T017清单；缺失或不匹配为preflight错误，禁止退回Python或另找PATH程序。T014白名单新增专门的`assembly-worker`子角色，必须由声明Provider派生且只能运行固定binary；worker网络连接和再exec始终拒绝。这条具名子进程边界替代旧Python IPC，不保留dual helper选项。

## Manifest Preservation

OA09逐字段复用旧native_assembly_helper._run：schema、modelName、modelDigest、modelManifestDigest、artifactProfileDigest、graphDigest、role、roleKind、rank、layerBegin、layerEnd、recipeDigest、adapterDescriptorDigest、assemblerDescriptorDigest、backendAbi、precision、quantization、layout、padding、inputNames、outputNames、nodeCount、entryDigests={model.onnx:actualDigest}、entryLengths={model.onnx:actualBytes}、onnxChecker=PASS、onnxRuntimeLoad=PENDING_NATIVE_PROVIDER、signer、layoutMode=INLINE_ONNX。key排序、compact UTF-8 JSON与原字节一致；不把worker已经CPU加载改写为Provider实际加载PASS。signManifest签实际manifest bytes，保留原缓存冲突/保护metadata。

## Remaining Closure and Proof

O-002设计关闭：4个原生ONNX字节探针、24个原numeric和14个扩展稳定identity参考、缺陷诊断及v2手工expected均已保存；完整算法/worker/manifest与[initializer normalization](initializer-normalization.md)的稳定编码、旧工件拒绝/重新导出规则已冻结。下面早先发现的缺口现在由T003/T006具名实现与T016证明承接，不能把设计关闭记为缺陷已修复或产品PASS。O-004完整迁移/API清单仍阻塞T001。

T001新增`tests/fixtures/spec182/dependency-probes/generate-identity-vectors.py`及`identity-vectors.json`：离线原版本reference探针，使用固定的小ONNX模型，冻结普通数值initializer的raw/typed等价identity；单独诊断BFLOAT16 raw与STRING identity稳定性，不把不稳定值存为正常oracle。`reference_case(name,dtype,values,raw) -> dict`生成owned模型/预期dtype/content和identity；`main()`固定onnx1.17.0、调用现有canonical_onnx_identity并输出JSON，不调用planned native实现。输出包含reference源码hash和模型hex；进程内外重复诊断只记录摘要/一致性，不输出未初始化内存原文。T001只做有界reference提取，T006用冻结稳定向量验证原生实现。

probe私有函数：`digest(data: bytes) -> str`返回canonical SHA256；`inspect_tensor(tensor: TensorProto) -> dict`构造固定Identity小图、full checker、临时模型文件，调用旧identity，返回model hex/digest、两个identity和tensorIndex，退出移除自己temp目录；`diagnostic_case(kind: str) -> dict`只接受bfloat16-raw/bfloat16-typed/string，用固定值对比内容摘要，返回dtype/modelDigest/observed与expected digest，不返回未初始化原文。ROOT和reference source path只定位旧实现；这些工具不安装进runtime。
generator同时硬校验NumPy1.24.4与旧graph.py SHA256=`e5532328e8752bd626b5a668de8e4b826ee9d336367a091fdd7016f48480734d`，旧源修复后拒绝重生成v1；新规范必须另立明确版本的reference，不能覆盖此次独立oracle。

R1 reference提取exit0：float32/float16/float64、int64/int32/int16/int8、uint64/uint32/uint16/uint8、bool共24例，12对raw/typed的graphDigest与initializerDigest分别相同，contentDigest与显式little-endian输入bytes一致。两次独立diagnostic确认BFLOAT16 raw摘要与已知bits不符（typed两次正确）；STRING同一model digest产生不同content digest。BFLOAT16这两次结果相同，故只声称内容错误，不声称已动态证明其每次随机。完整摘要与版本/source hash见[identity reference evidence](../evidence/identity-reference-20260906.json)。原生runner当前I/O映射float32/float16/int64不等于initializer格式全集，不能据此随意删除内部dtype支持。

S4缺陷已由独立诊断确认并冻结修正规则：BFLOAT16 raw、STRING以及typed-complex不得按旧错误路径迁移。T003和T006消费initializer-normalization的v1/v2契约；recipe/manifest的字段及canonical JSON复用原结构，正式unit必须冻结并检验实际wire，不能用本次仅identity数据替代。普通已维护numeric行为不得降级，旧错误digest不允许fallback。

T006的`tests/unit-tests/di-native-assembly.t.cpp`必须区分：exact byte/identity/recipe/manifest、local function、inline/external别名、wrong offset/size/identity/node/I/O、cancel-before-start/during-worker/after-response、child crash/partial/oversize frame和warm cache不能绕过授权。超时反例用故意阻塞的测试worker，在启动前登记其测试身份，不在生产暴露任意worker路径。T015审查父子边界，T016真实冷recipe/保护/cleanup及no-Python反例；探针不计这些产品证明。
