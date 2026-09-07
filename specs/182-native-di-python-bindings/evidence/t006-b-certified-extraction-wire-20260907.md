# T006-B Certified Extraction and Wire — 验收执行证据（2026-09-07）

Status: DONE。Selector：CPP(Spec182OnnxExtraction/*)。新承载 suite 建于
[tests/unit-tests/di-native-onnx-recipe.t.cpp](../../../tests/unit-tests/di-native-onnx-recipe.t.cpp)
（case-manifest 已按 suite 文件决策将该 suite 登记到该 planned file；11 cases：
4 accept + 7 reject，全部以冻结 python 参考的 expected bytes/digests 断言，
expected 绝不由本 assembler 生成）。既有 suite 回归：
Spec182NativeAssembly 3 cases（含 InlinesNestedGraphExternalInitializers 适配为
checker-valid If fixture）、Spec182OnnxIdentity 11 cases。execution-units 写卡
Write 文件行与 manifest 文件差异按 registry 约定以 manifest 为准（后续
T006-C/D 的 U/I 行按 execution-units.md 落在同一 t.cpp）。

## 执行命令与结果

```
python3 tests/fixtures/spec182/dependency-probes/generate-extraction-vectors.py   # 冻结 python 参考重生成
#   frozen 参考 onnx 1.17.0 / ort 1.19.2 / np 1.24.4 / python3.8
#   FROZEN vectors sha：6f289a0175d11444501df0b3b3cef750cbf7ed57 -> 77300e135a960e8c8cb26c9500e2058934ef47a87aabb19191cffafee289330e
./waf -o build-nac182 build --targets=unit-tests -j2       # rc=0（多次迭代，零自身 warning）
./build-nac182/unit-tests --run_test=Spec182OnnxExtraction,Spec182NativeAssembly,Spec182OnnxIdentity   # rc=0，25 cases
./build-nac182/unit-tests                                  # 完整回归：5 个环境性失败（下详），与本卡无关
```

raw 输出见 `.codex-tmp/t006b-*.log`（reconfig/build/suite-run1..4/full-regress、
vectors-regen.{json,err}）。

## 卡 Steps 对照

- **官方 ONNX 1.17 full-protobuf 统一**：assembler TU 与 suite 均经
  `--onnx-prefix` wscript option 消费 onnx-install（include/onnx、
  lib/libonnx.a+libonnx_proto.a；`-DONNX_ML=1 -DONNX_NAMESPACE=onnx`）。
  include 约定：只包含 `<onnx/onnx_pb.h>`（先定义 ONNX_API 再拉 onnx-ml.pb.h），
  绝不直接包含 pb header。
- **S3 全量 checker on inlined original**：`onnx::checker::check_model(original,
  true)` 于内联后、证书比较前运行；checker 拒绝 -> DI_NATIVE_ONNX_GRAPH。
- **S4 identity digests == recipe**：graphDigest/canonicalInitializerDigest 与
  冻结 recipe 逐字相等，mismatch -> DI_NATIVE_ONNX_RECIPE。
- **S5 InferShapes on copy + 逆向 DFS 抽取**：copy 上 InferShapes（原图不变），
  从 certified outputs 逆向 reachability，按原序重建 extractor metadata
  （referred local functions 一并在列）；reject rows 见下映射。
- **S6 全量 check assembled + node cover/逐节点确定性字节 + io dtype/shape**：
  assembled 再过 full checker；node 数与 certified indices 一致且逐节点
  deterministic bytes 相等（-> NODE_COVER）；boundary io 与 contracts 语义
  比较（missing -> IO_CONTRACT / dtype+shape -> IO_DTYPE）。
- **S7 确定性 wire <= maxAssembledBytes + 真实 ORT CPU session load**：
  wire 为 deterministic message bytes；随后以该确切字节建 Ort::Session
  （ORT_ENABLE_ALL，1.26.0，destruction 在结果离开前）。C++ ORT 1.26.0 与
  python 参考 ort 1.19.2 的版本差已记录（python 冻结阶段 1.19.2，native 会话
  加载 1.26.0；两者对 4 条 accept wire 均加载成功）。

## Reject 映射（frozen 文本 -> native reason family）

| row | poison | python 阶段 | native gate |
|---|---|---|---|
| r4 | recipe digest 不符 | digest mismatch 文本 | RECIPE (S4) |
| r5 | ghost output io | adapter-certified extraction failed | GRAPH (S5 boundary) |
| r6 | node cover 缺 | cover differs from certificate | NODE_COVER (S6) |
| r7 | ghost input | recipe 构造期 IO_CONTRACT 冗余交叉检查 | GRAPH (S5 boundary；native 无 analog，见下) |
| r8 | io dtype poison | dtype/shape mismatch | IO_DTYPE (S6 compare) |
| r9 | io shape poison | dtype/shape mismatch | IO_DTYPE (S6 compare) |
| r10 | layer escape | node cover escapes the graph | LAYER_RANGE (entry) |

r7 差异的正当性：python 在 recipe 构造期用冗余的 input_names/contracts 交叉
检查拒绝（IO_CONTRACT 文本）；native 的 NativeSelectionRoleV3 没有 io-name
members（io names 只存在于 contracts），同一 poison 在 S5 的 certified
boundary 无法 bind 到 graph 时以 GRAPH 浮出——同一语义家族的等价拒绝。

## 关键发现与修正

1. **proto3-optional data_location 字节奇点（byteParity 修正）**：
   onnx.proto `optional DataLocation data_location = 14` 是显式存在性字段，
   explicit DEFAULT 会写出 wire tag 0x70（"7000"）。python-upb loader 与 C++
   full protobuf 在 parse 时都保留该 tag（has_data_location() 恒 true）。
   因此 external 源行内联后与 python 冻结 wire **逐字节相同**：frozen
   byteParity=False 标志过度保守，已更正为 True，generator 注释同步更新，
   suite 对应断言从 semantic-only 升级为 byte equality（另加独立
   sha256(modelBytes)==modelDigest 交叉检查，4 条 accept 全过）。
   vectors 重新冻结：sha 6f289a01… -> 77300e13…（唯一 delta：该行
   byteParity False->True，其余 11 行逐字节不变，已 diff 验证）。
2. **nested-external If fixture**：If 属性必须显式 set_type(GRAPH)（C++ 默认
   UNDEFINED，S3 full checker 拒绝）；bool 标量 cond 必须带零维 shape
   （full checker：io value 的 type shape 字段必填）—— python 镜像逐 stage
   复现后确认，C++ 端到端（checker+InferShapes+ORT）与 python 1.17 行为一致。
3. **全量回归 5 失败与本卡无关**（环境性、pre-existing）：`sign`/StreamFacade
   TPM-PIB missing private key 与 NFD 环境依赖，路径全部在 ndn-cxx/NFD，
   ONNX 改动不可达。
4. 冻结 python 参考、python 侧 EXTERNAL full-check 限制、C++ 与 python 对
   If 的构造细节差异等已在 suite 头注释与 generator 中留痕。

## 文件

- [di-native-onnx-recipe.t.cpp](../../../tests/unit-tests/di-native-onnx-recipe.t.cpp)（new，Spec182OnnxExtraction 11 cases）
- [di-native-assembly.t.cpp](../../../tests/unit-tests/di-native-assembly.t.cpp)（If fixture 适配 + cond scalar shape）
- [generate-extraction-vectors.py](../../../tests/fixtures/spec182/dependency-probes/generate-extraction-vectors.py)（byteParity 修正 + 注释）
- [extraction-vectors.json](../../../tests/fixtures/spec182/dependency-probes/extraction-vectors.json)（重新冻结，sha 77300e13…）

残余风险：frozen vectors 覆盖 sparse/quant 之外的代表性形状但非穷尽；
C++ ORT 1.26.0 与 python ort 1.19.2 的运行时行为差按证据文档留档。
