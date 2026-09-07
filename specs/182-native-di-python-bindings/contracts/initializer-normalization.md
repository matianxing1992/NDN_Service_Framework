# Initializer Normalization and Compatibility

**Status**: DESIGN_FROZEN / T001; production correction NOT_STARTED
**Finding**: A7-07; [original evidence](../evidence/identity-reference-20260906.json)
**Owner**: T003 graph preparation/offline exporter; T006 native assembly identity; T016 mixed-version proof

## Decision

原始24个稳定numeric向量和BFLOAT16 typed身份继续有效。修复BFLOAT16 raw_data被忽略与STRING object-array指针字节参与摘要的问题；不复现未初始化内存，也不接受错误摘要作为“兼容模式”。修订名为`ndnsf-di-initializer-normalization-v2`，它是算法/交付身份，不新增网络服务或第二套认证状态。

graph facts结构、recipe/root wire schema以及SHA256算法不变。只有原本有缺陷的BFLOAT16表示和STRING规范化内容改变；STRING包括空张量统一使用新格式。原有无此类initializer的模型应逐字节保留所有identity字段。涉及变化的manifest/profile/recipe必须由原发布流程重建并重新签名/授权，不能在接收端替换已签名digest。

## Exact Source Changes

| Operation / path | Symbol / responsibility / parameters |
| --- | --- |
| MODIFY `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/onnx/graph.py` | ADD `_initializer_identity_payload(initializer) -> tuple[str, tuple[int,...], str, bytes]`：只读TensorProto，返回dtype、shape、byteOrder、规范内容；非法形状/编码抛ValueError。MODIFY `canonical_onnx_identity`：用该返回值替换numpy数组归一化小段，保留原排序、别名、graph/semantics/parameter digest构造；不改公开签名或CanonicalOnnxIdentity字段 |
| same | ADD `_initializer_normalization_revision(model: ModelProto) -> int`：只检查参与tensorIndex的顶层initializer；STRING、BFLOAT16 raw/external、COMPLEX typed任一存在取2，否则1。ADD `certified_onnx_assembler_digest(path: str|Path) -> str`：load_external_data=False读取源model，调用revision helper并返回下方确切descriptor digest；不访问权重对象、不签名 |
| MODIFY `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/yolo/adapter.py`, `examples/python/NDNSF-DistributedInference/llm_pipeline/user.py` under T003/T012 | YoloCanonicalArtifactBinding的binding和Qwen canonical binding的_assembler_digest使用上述factory替代无条件v1常量；普通模型仍返回v1。这些maintained caller随后按T012迁移为native facade，不保留第二套分类算法 |
| MODIFY planned `NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.cpp` | OA06的`initializerIdentityPayload(const onnx::TensorProto&) -> NormalizedInitializerPayload`实现同一规范；由canonicalIdentity调用，格式操作不读取文件/网络、不含Python |
| ADD private type in same planned C++ module | `NormalizedInitializerPayload { string dtype; vector<int64_t> shape; string byteOrder; vector<uint8_t> content; }`；empty初始，构造完整后才能返回；content请求期owner/RAII，无全局缓存 |
| MODIFY `tests/python/test_spec170_canonical_layers.py` under T003 | 增加具名normalization与packing等价回归；原24个v1 numeric expected只读，原graph.py版本参考generator保持锁定并拒绝覆盖 |
| ADD fixture metadata under T001 | `tests/fixtures/spec182/dependency-probes/initializer-stable-v2-vectors.json`：根据以下字节规则手工编码expected，不调用planned helper产生expected。另冻结原版本complex/custom numeric向量到`identity-extended-vectors.json`，不改原24向量 |

Python修正是保留的离线导出/参考路径的必要一致性工作，T003明确拥有；182默认runtime仍必须在T006/T010用原生实现。不能把“Python参考修好了”作为C++迁移完成。`_initializer_identity_payload`不接收版本/allowLegacy/trusted开关，避免调用者选择不稳定路径。

## Payload Rules

shape为TensorProto.dims原序，所有维度非负，elementCount为带溢出检查的乘积；标量空shape的count=1，有任一零维count=0。count必须匹配实际编码；不能用未初始化内存补缺失元素。上层source/资源预算仍适用。
格式错误统一为`DI_ONNX_INITIALIZER_ENCODING_INVALID`，Python helper用带该原因码的ValueError，native adapter/worker映射为同码的adapter错误；不以错误文本替代协议版本或认证结论。

| Data type | dtype / byteOrder | Canonical content |
| --- | --- | --- |
| 既有普通numeric | 原NumPy1.24.4/ONNX1.17 dtype标签；itemsize>1为little，否则na | 保留原little-endian contiguous编码；raw和typed表示规范化后相同，不进行数值精度变换、NaN规范化或signed-zero折叠 |
| COMPLEX64/COMPLEX128 | complex64/complex128 / little | raw精确为8或16×count字节；typed取float_data/double_data中严格2×count个分量，逐对real/imag按little-endian拼接。旧1.17 typed转换已观察TypeError，不能先组Python complex再转float storage；v2按原raw规范修正 |
| BFLOAT16 | `(numpy.uint16, [('bfloat16', '<u2')])` / little | raw存在时精确要求2×count字节并直接采用；无raw时要求int32_data长度=count，每项取低16位按little-endian拼接。保留NaN payload及正负零位模式；不能经float32数值转换。混合互斥data字段由checker/格式检查拒绝 |
| FLOAT8E4M3FN/FLOAT8E4M3FNUZ/FLOAT8E5M2/FLOAT8E5M2FNUZ | 原custom dtype标签 / na | 原8-bit位模式，raw或int32_data低8位；精确count，不改变NaN位模式。以额外冻结原版本向量确认标签与编码 |
| UINT4/INT4 | 原custom dtype标签 / na | packed字节low nibble在前，展开为count个u8或符号扩展i8；raw/typed packed表示一致，奇数count最后高nibble不是额外元素。源packed长度须为ceil(count/2) |
| STRING | string / na | 以下明确的UTF-8 framing；不用NumPy object内存，不做Unicode NFC/NFD转换 |

STRING content精确为：ASCII `NDNSF-ONNX-STRING-v2`加一个NUL字节，随后uint64 little-endian elementCount；再按row-major顺序，每项uint64 little-endian UTF-8 byteLength后紧跟原始UTF-8 bytes。空字符串长度0；字符串里的NUL是内容字节；非法UTF-8拒绝。tensor.string_data长度必须等于elementCount，不接受raw_data替代STRING编码。`byteLength`为整个framing长度，contentDigest为这些bytes的SHA256。

所有type的tensorName/shape/dtype/byteOrder/byteLength/contentDigest照原tensorIndex字段写入，sharedReference继续由完整contentDigest分组且按name排序。STRING framing不会把`["a","bc"]`与`["ab","c"]`混淆；shape仍进入独立layout，内容相同但shape不同不代表完整模型身份相同。

原custom dtype字面值固定如下，来自本次独立1.17/1.24.4参考提取；C++不能自行改成更好看的类型名，否则改变graph digest：

| ONNX type | Exact dtype label |
| --- | --- |
| FLOAT8E4M3FN | `(numpy.uint8, [('e4m3fn', 'u1')])` |
| FLOAT8E4M3FNUZ | `(numpy.uint8, [('e4m3fnuz', 'u1')])` |
| FLOAT8E5M2 | `(numpy.uint8, [('e5m2', 'u1')])` |
| FLOAT8E5M2FNUZ | `(numpy.uint8, [('e5m2fnuz', 'u1')])` |
| UINT4 | `(numpy.uint8, [('uint4', 'u1')])` |
| INT4 | `(numpy.int8, [('int4', 'i1')])` |

R2扩展提取16例：14个稳定identity成功，complex64/128 typed两例分别保留旧TypeError，不标成成功。新增v2手工goldens为12个正例和5个明确拒绝例；已独立解析所有STRING framing、复算payload hash，并证明新typed-complex payload与原raw identity内容摘要一致。这里验证的是expected工件正确性，5个拒绝例及新生产normalizer均待T003/T006真正执行。

## Compatibility Matrix

| Source / stored identity | Native v2 disposition | Export / rollback |
| --- | --- | --- |
| 原24个普通numeric、原有稳定raw complex/custom numeric | 必须等于冻结v1 identity；原recipe/manifest/grant绑定继续验证 | 不强迫全部模型重导出；旧向量永不更新 |
| 旧typed-complex转换失败 | 新转换按对应raw bits产生稳定identity；不伪造旧程序曾成功 | 字段语义不变，失败的旧导出需重新执行；旧版本不保证能接受typed表示 |
| BFLOAT16 typed有效完整bits | 必须保留旧稳定摘要 | raw/typed/external packing在v2统一；不改变数学权重 |
| BFLOAT16 raw的旧摘要与真实bits不符 | 在graph/initializer digest边界拒绝 | 重新导出profile/manifest/recipe并重新签名授权；不把旧错误摘要映射到新摘要 |
| STRING旧object-array identity（包括旧空tensor布局） | 按新规范重算；不相等就显式拒绝，不fallback到指针摘要 | STRING规范变化明确要求重新导出；旧程序不得宣称可读新身份。回退使用此前完整版本/工件集并保留其UNQUALIFIED事实 |
| 不完整BFLOAT16/packed数据、无效UTF-8或count不符 | 格式边界拒绝，没有成功identity | 修复源工件，不能调宽shape/count或用padding掩盖 |

算法revision写进182构建/交付身份及更新后的adapter/assembler descriptor实现版本记录，使用既有descriptor digest绑定，不改变已冻结签名/密文。跨版本测试区分“普通numeric旧新互通”与“修正表示需要新发布”，不得把全部格式宣称为无条件混用。NativeProvider真实权限、ControllerVersion和grant校验全部保留。

具体使用既有`assemblerDescriptorDigest`字段，无须新增root wire字段：

| Revision | Descriptor preimage / exact digest |
| --- | --- |
| 1 | ASCII `ndnsf-di-certified-onnx-assembler-v1` → `sha256:f0210ce34f62d5fdabcd8129a0dfbafcf8fca8d99852443518b5fcda7434841b` |
| 2 | ASCII `ndnsf-di-certified-onnx-assembler-v2` → `sha256:5291ee00f425c59605f72e26c9b27a73aca43b976421218515fc1a38085c7a89` |

Native graph/preparation owner用同一分类规则选取descriptor并参与原有recipe/plan封存；T008的实际adapter调用映射由O-004清单冻结。OA04在源内联后检查需要v2的表示：若已绑定recipe不声明上述v2 descriptor则报`DI_ONNX_NORMALIZATION_REVISION_REQUIRED`，不能替换descriptor继续执行。普通numeric保留此前受信任recipe的descriptor校验规则，不额外禁止既有合法adapter描述；v2内容必须有明确v2绑定。BFLOAT16 external内联后成为raw因此属于v2，COMPLEX external成为合法raw仍属v1；STRING空张量也属于v2。typed-complex源的full checker成功不等于旧转换成功。新增`test_initializer_normalization_descriptor_selection`覆盖这些分支。

## Selectors and Closing Proof

T003新增`test_initializer_v1_numeric_vectors_unchanged`、`test_bfloat16_raw_typed_identity_equal`、`test_string_identity_framing_vectors`、`test_string_identity_process_independent`、`test_initializer_malformed_encoding_rejected`，同时保留原canonical packing回归。T006在di-native-assembly单测消费同一v1/v2 goldens并验证recipe/manifest；T016真实Provider验证旧错误digest拒绝与新导出身份接受。T001只冻结设计及独立expected，不把这些尚未实现的selector标PASS。

扩展reference R1在complex64-typed的旧numpy_helper转换处失败（full checker已通过），单独保留failure index。新增`tests/fixtures/spec182/dependency-probes/generate-extended-identity-vectors.py`：`main()`锁定同一旧源/依赖，独立运行16种complex/custom表示，逐例保存成功identity或旧转换错误；只有预先识别的typed-complex TypeError可作为已知诊断，其他错误仍导致非零退出。fixture的accepted=false不是产品能力豁免。新增selector `test_complex_raw_typed_identity_equal`归T003/T006验证修正。
