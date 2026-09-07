# T006-A Canonical Source Identity — 验收执行证据（2026-09-07）

Status: DONE。Selector：CPP(Spec182OnnxIdentity/*)，承载 suite 建于 manifest 登记文件
[tests/unit-tests/di-native-assembly.t.cpp](../../../tests/unit-tests/di-native-assembly.t.cpp)
（existingSuite `Spec182NativeAssembly`、existingCases 3 原样保留并回归通过；
新 suite `Spec182OnnxIdentity` 11 cases；layers L1/2/6，L6 executeOwner T016——
真实 Provider 冷装配消费由 T006-D/T016 运行）。execution-units Write 的
U/di-native-onnx-recipe.t.cpp 与 manifest 文件差异按 registry 约定以 manifest 为准
（该文件已注册进 unit-tests ant_glob，无 wscript 改动；new-file 拆分在 T006-B/C
再评估）。

## 执行命令与结果

```
./waf -o build-nac182 build --targets=unit-tests -j2       # rc=0（含 seam TU 重编，无警告）
build-nac182/unit-tests --run_test=Spec182OnnxIdentity     # rc=0，11 cases
build-nac182/unit-tests --run_test=Spec182NativeAssembly   # rc=0，3 cases（既有回归）
```

raw 输出见 `.codex-tmp/t006a-identity.log`、`.codex-tmp/t006a-regression.log`。

## 卡 Steps 对照

- **owned source**：`canonicalOnnxSourceIdentity` 只对调用者传入的
  `NativeCanonicalSource{modelBytes, initializerBytes?}` 字节工作；assembly
  control（deadline/requireActive/maxSourceBytes/maxAssembledBytes）在解析与
  内联前后分别栅栏。零 limit/空 bytes/超限 bytes/垃圾 bytes 分别
  `DI_NATIVE_ONNX_SOURCE_LIMIT`/`DI_NATIVE_ONNX_PARSE`。
- **内存 external 校验（不从 external location 打开文件）**：deep scan 覆盖
  顶层/嵌套 graph initializer、node attribute tensor、function node attribute
  tensor（S2 `_get_all_tensors` 语义，FunctionAttributeExternals 测试证明）；
  location 必须同相对路径（绝对/`../`/空组件拒绝 `DI_NATIVE_ONNX_EXTERNAL_
  LOCATION`）、offset 有界（超界 `DI_NATIVE_ONNX_EXTERNAL_RANGE`）、无 length 与
  length "0" 均为 rest-from-offset（1.17 loader 规则）；binding 存在 iff 有
  external（`DI_NATIVE_ONNX_EXTERNAL_BINDING` 双向拒绝）；有 external 时仅从
  内存 binding 拷贝内联——全路径无任何按 model 内声明路径的 file open。
- **原图 identity**：identity 在 parse 后、任何 shape inference 之前的原图
  上计算（本 seam 无任何 inference 调用，shape-inferred 图不可能参与）；
  graphDigest 为 graphFactsJson 的 sha256，initializerDigest 为逐 initializer
  规范化 payload 摘要串——组合逐字节复现冻结 python 参考
  graph.py::canonical_onnx_identity（见下）。private 组成函数留在模块 TU
  anon namespace。
- **版本化 normalization**：normalizedOnnxInitializerPayload 以序列化
  TensorProto 字节为输入的同 TU seam 导出（hpp 仅 vector/string 签名，
  无 ONNX/protobuf 类型进公共头/安装面）；v1/v2 判别按
  onnxInitializerNormalizationRevision（STRING、BFLOAT16 raw、typed COMPLEX → 2，
  其余 1，external 先内联再分类）；checkOnnxAssemblerDescriptorBinding 在
  rev-2 源未绑 v2 descriptor（sha256:5291ee00…）时拒绝
  `DI_ONNX_NORMALIZATION_REVISION_REQUIRED`，rev-1 源保持 legacy descriptor
  （sha256:f0210ce3…）规则——错 descriptor 拒绝属本卡验收面。
- **复用 ONNX/protobuf**：沿用 onnx-ml.pb.h（lite runtime）+ 既有 wire
  工具；normalization 错误族边界：`DI_ONNX_INITIALIZER_ENCODING_INVALID`
  覆盖元素计数/域编码违规，`DI_NATIVE_ONNX_*` 只用于源/binding 边界族。

## Verify 对照（Spec182OnnxIdentity 11 cases）

- 冻结 golden 全量复现：`WholeModelNumericVectorsMatchFrozenGoldens`（identity-
  vectors.json 24 全模型：contentDigest/graphDigest/initializerDigest/modelDigest/
  modelHex/tensorIndex 全字段）；`WholeModelExtendedVectorsMatchFrozenGoldens`
  （extended 16：14 accepted 全 golden + 2 complex-typed legacy TypeError 拒绝
  逐字面一致）；`RawAndTypedPairsProduceIdenticalDigests`（每 dtype typed/raw
  pair 摘要相等——fixture 冻结的 packing 恒等）；`V2PerTensorVectorsMatchHand
  EncodedPayloads`（stable-v2 17：12 accepted payloadHex/dtype/shape/byteOrder/
  byteLength/contentDigest 全逐字节一致 + 5 拒绝 DI_ONNX_INITIALIZER_ENCODING_
  INVALID）；`V2Bfloat16RawAndTypedShareOnePayload`（bf16 两编码归一）。
- normalization 契约：`RevisionClassificationFollowsNormalizationContract`
  （STRING/BF16-raw/typed-complex→2，bf16-typed/complex-raw/float-raw→1，
  external-inlined 参与分类）；`AssemblerDescriptorBindingGate`（rev-2 缺
  v2 descriptor 拒绝、rev-1 兼容）；`TypedComplexIsV2BoundNotLegacyConvertible`。
- external/内存规则：`ExternalInliningRulesAndInlineEquivalence`（offset+无
  length / length "0" / 绝对路径 / `..` / offset 超界 / 双向 binding 缺失 /
  双 location 拒绝，external 内联 identity == 直接内联 identity）；
  `FunctionAttributeExternalsAreValidatedAndInlined`（function node attribute
  tensor 被 deep scan，内联后 identity 与 inline 模型逐字节一致）。
- 边界与 overflow：`IdentityRejectsBoundaryViolations`（零/超 source limit、
  side-file 超 initializer limit、垃圾 parse、2^62×2^62 dim 积 overflow 与
  负 dim → INITIALIZER_ENCODING_INVALID）。
- 私有算法同 TU seam、类型不公开安装：suite 与模块 TU 一样只通过 hpp 四个
  seam（vector/string/整数签名）驱动；onnx::TensorProto 只出现在模块 TU 与
  测试 TU（fixture 构造），头文件无 onnx 类型 include（T002-A 安装面不变）。

## 实现中的三处修正（均有绿色结果兜底）

1. **graphFactsJson hex 缺 JSON 引号**：values inputs/outputs 与 node attribute
   "wire" 曾以裸 hex 出串（captured dump 证 `[0a05…` 无引号），python 参考以
   quoted JSON string 输出 → 与冻结 graphDigest 全数不符。改为 jsonString 包裹
   （helper 拆为 protoHex + jsonHexBytes）；value_info/functions 走既有 sorted
   list 引用路径不动。修正后 24+14 全模型 graphDigest 全对。
2. **INT4/UINT4 raw 展开**：raw 4-bit 字节须按 low-nibble-first 展开到
   count 字节（INT4 符号扩展），与 typed 编码器一致——generator 冻结
   `UINT4 [0x73]→[3,7]`、`INT4 [0x7d]→[0xfd,7]` 位型为证；此前 verbatim
   packed 与 golden 不符。
3. **测试侧两处**：ExternalInliningRulesAndInlineEquivalence 的 weights 向量
   改为 offset 2 + 16 content 字节（原 24 字节使 rest-from-offset 长度校验
   失败）；IdentityRejectsBoundaryViolations 的 initializer-limit 用例改为
   模型按其自身字节数过 source 门 + 1 MiB side file 精确触发
   INITIALIZER_LIMIT（原 control 2 使 SOURCE_LIMIT 先发）。两处均只改测试，
   不动生产语义。临时 NDNSF_DEBUG_FACTS dump 已删除，重编无警告。

## 修改

- NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.cpp：
  四个 seam 实现 + private 组成（deep external scan/inline、原图 identity JSON
  组合、v1/v2 规范化编码、revision 分类、descriptor binding 门、限额/active
  栅栏）。
- NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp：
  四个 seam 声明（无 onnx/protobuf 类型）。
- tests/unit-tests/di-native-assembly.t.cpp：suite `Spec182OnnxIdentity` 11
  cases + helpers（fixtureRow/checkWholeModelGolden/makeTensorFromSpec/
  singleInitializerModel/expectPrefix 等）；既有 3 cases 未动。
- 无 wscript 改动（manifest 文件已注册）；fixtures 保持冻结未改。
