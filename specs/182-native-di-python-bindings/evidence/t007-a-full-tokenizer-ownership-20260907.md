# T007-A Full Tokenizer Ownership — 验收执行证据（2026-09-07）

Status: DONE。Selector：CPP(Spec182NativeTokenizer/*) + CPP(Spec182TokenizerFull/*)。
实现：NativeTokenizer 从 dlopen bridge 切换为对冻结 HF 核心五函数 ABI
（create/encode/decode/free/destroy，稳定四字节 id 布局）的直接静态链接调用；
pinned Rust tokenizers crate 以 staticlib（release，--locked --offline -j2，
独立 target 目录）经 waf TOKENIZER_BRIDGE uselib 链入 DI 共享库与全部 C++
link consumers；`bridgeLibrary`（旧 dlopen 路径）从 factory options 移除，
NativeStandaloneTokenizer factory 直连 engine 完整 decode 与 factory 兼容签名
保留（digest-first 2-arg NativeTokenizer ctor 不变）。

## 执行命令与结果

```
./waf -o build-nac182 configure --prefix=.../.codex-tmp/spec182-t006c-l0/staging \
    --with-tests --nac-abe-prefix=.../nac-abe-integration-182/install \
    --onnx-prefix=.../.codex-tmp/spec182-t001-dependencies/onnx-install \
    --ndn-svs-source-tree=/home/tianxing/NDN/ndn-svs \
    --ndn-svs-build-tree=/home/tianxing/NDN/ndn-svs/build
#   rc=0；'Pinned Rust tokenizer staticlib : .../libndnsf_tokenizer_bridge.a'
#   （t007-configure-r2.log）
./waf -o build-nac182 build -j4        # rc=0；640 tasks 1m15s（t007-build-r3.log）
./build-nac182/unit-tests --run_test='Spec182NativeTokenizer/*'
#   rc=0；3/3 全绿（digest-first engine 门、artifact 早拒、factory 身份强制）
./build-nac182/unit-tests --run_test='Spec182TokenizerFull/*'
#   rc=0；7/7 全绿，84 向量对照 + ABI 负路径 + owner 复用 + 并发串行化
./build-nac182/unit-tests --run_test='!StreamFacade'   # 完整回归 859 cases（852+7），No errors detected
#   （t007-full-regression.log）
#   注：裸跑（不带排除）在已知环境性 StreamFacade 族
#   PredictiveProviderExactWireValidationAndAtomicFlush 段错误 rc=139（负结果
#   已记 failure-log；T006-B/C/D 同族同样排除）
```

raw 输出见 `.codex-tmp/t007-*.log`（configure-r2、build-r3、full-regression、
早期失败 configure/build-r1）。

## 卡 Steps 对照

- **迁入锁定 HF 核心的五函数 ABI，C++ digest/RAII/mutex 与完整
  encode/decode factory；同 allocator 释放**：NativeTokenizer.cpp 完全重写
  —— Impl{digest, handle, mutex} 先 sha256File 校验 expectedDigest
  （mismatch 于 engine create 前抛 invalid_argument），再 readBytes 过
  ndi_token_create；返回错误时 errorText 用 ndi_token_free 释放（同
  allocator 律），所有 result.data 恰好释放一次；encode 限制 1 MiB、
  checkedIds 把 int64→u32（负/超界在 ABI 前拒），decode/decodeStable 分派
  五参 stable 变体；析构 RAII destroy 并持锁。NativeTokenizer.hpp 文档改
  为 statically linked（spec182 T007）。
- **不升级 crate、不装 probe 库**：无任何 Cargo.toml/Cargo.lock/依赖变化，
  无新 dlopen；pin 记录即 case-manifest 的 rust 1.90.0 prefix/cargo-home。
  `.a`（38,803,430 B）pre-built 于 T001 依赖安装，waf helper 只在 crate
  源码新于 archive 时重跑 cargo。
- **保留原完整 factory 兼容签名**：NativeStandaloneTokenizerOptions 移除
  bridgeLibrary（冻结文档从未出现该字段）后仅剩 tokenizerPath；factory
  签名/返回类型与行为契约不变，makeNativeStandaloneTokenizerDecoders 无
  identity 时依旧 invalid_argument。
- **每编译 NativeTokenizer.cpp 的 target 都链 archive**：root waf
  DI shlib use 追加 TOKENIZER_BRIDGE（STLIB_TOKENIZER_BRIDGE /
  STLIBPATH_TOKENIZER_BRIDGE，渲染 `-Wl,-Bstatic -L<dir> -lndnsf_tokenizer_bridge`
  由 gcc.py STLIB_MARKER 机制实现）；tests/wscript framework_use +
  unit-tests use、examples/wscript 7 个 DI targets 同步追加（spec182-
  installed-consumer 只链 shlib，无源码 TU）。

## 卡 Verify 对照

1. **84 完整对照**：冻结 vectors.json（schema v1，reference tokenizers
   0.20.3，ensure_ascii=False）三 fixture legacy-ascii / legacy-unicode /
   byte-fallback-special × 28 cases，tokenizerJson 物化到
   /tmp/spec182-tokenizer-full-<pid>/ 并先校验 sha256 再 open（digest
   匹配即证明引擎读到的就是冻结字节）；encode 逐元素 EQUAL_COLLECTIONS
   对照 ids、decode 全等对照 decoded —— 全部一致。
2. **special flags**：28 cases/行各带 addSpecial/skipSpecial 组合，全通过；
   OwnerReuse 另验证 skipSpecial=true/false 两路径 5 轮稳定。
3. **错误 ID / UTF-8 / digest**：负 id 与 >u32 id 于 ABI 前 invalid_argument；
   范围内但词表外 id → engine 错误码 → runtime_error；无效 UTF-8 → engine
   encode 错误 runtime_error；>1 MiB → invalid_argument；digest mismatch /
   artifact 缺失均在 engine create 前拒绝。
4. **owner 复用与串行调用**：同 owner 重复 encode/decode/digest 5 轮
   确定性一致；8 worker × 40 迭代并发调用共享 owner（互斥锁串行化），
   结果与基线逐次一致，无崩溃/无数据竞争症状。
5. **实际 native ABI 为被测对象**：全程走静态链接的真实 ndi_token_* ABI
   （create/encode/decode/decode_stable/free/destroy），非 mock、非 dlopen
   probe 路径。

## 关键发现与修正

1. **configure rc=2 NameError（wscript 结构损坏）**：`_ensure_tokenizer_bridge`
   曾插进 `_pin_compiler_toolchain` 体内，其尾部语句挂进 helper（`tools`
   未定义）。修复：helper 完整置于模块级、重建 `_pin_compiler_toolchain`
   并删除被遮蔽的残缺旧 def（只留一个带文档的完整版）；随后 configure rc=0。
2. **boost 1.71 不能流式打印 std::vector**：`REQUIRE_EQUAL(encode(), ids)`
   无法编译（print_helper 无 vector operator<<）。修复：三处改逐元素
   `BOOST_REQUIRE_EQUAL_COLLECTIONS`。
3. **case 名改名**：原 suite 3 case 后缀 BridgeLookup → EngineCreate
   （dlopen 时代措辞）；manifest existingCases 同步（文件顺序）。
   已登记 case-manifest 并在 [docs/failure-log.md](../../../docs/failure-log.md)
   （2026-09-07）记录完整诊断。

## 文件

- [NativeTokenizer.hpp](../../../NDNSF-DistributedInference/cpp/adapters/qwen/NativeTokenizer.hpp)、
  [NativeTokenizer.cpp](../../../NDNSF-DistributedInference/cpp/adapters/qwen/NativeTokenizer.cpp)
  （dlopen 移除 → 静态 ABI 直连；digest-first、RAII、互斥串行化；full
  decode factory 兼容签名保留）
- [NativeStandaloneTokenizer.hpp](../../../NDNSF-DistributedInference/cpp/ndnsf-di/NativeStandaloneTokenizer.hpp)、
  [NativeStandaloneTokenizer.cpp](../../../NDNSF-DistributedInference/cpp/ndnsf-di/NativeStandaloneTokenizer.cpp)
  （bridgeLibrary option 移除，factory 直连 NativeTokenizer(path, digest)）
- [wscript](../../../wscript)（`_ensure_tokenizer_bridge` helper +
  TOKENIZER_BRIDGE uselib env；DI shlib use 追加；_pin_compiler_toolchain
  单份完整化）
- [tests/wscript](../../../tests/wscript)、[examples/wscript](../../../examples/wscript)
  （TOKENIZER_BRIDGE 追加到全部 NativeTokenizer.cpp link consumers）
- [di-native-tokenizer.t.cpp](../../../tests/unit-tests/di-native-tokenizer.t.cpp)
  （Spec182NativeTokenizer 3 gate cases + Spec182TokenizerFull 7 cases）
- [case-manifest.json](../../../tests/fixtures/spec182/case-manifest.json)
  （T007-A existingCases 改名 + 7 named cases）
- [docs/failure-log.md](../../../docs/failure-log.md)（2026-09-07：wscript
  结构损坏 + boost vector 打印 + cargo PATH 教训）

残余风险：decodeStable 的 final=false 半稳定占位与 ByteLevel profile 稳定
算法属 T007-B 卡（本卡只验证稳定 ABI 可达、final=true 与 decode 全等）；
真实 Qwen ByteLevel 工件的端到端采样按卡约定延至 T016。Rust crate 侧
Cargo.toml/Cargo.lock 未变更（T007-A 无升级授权）。
