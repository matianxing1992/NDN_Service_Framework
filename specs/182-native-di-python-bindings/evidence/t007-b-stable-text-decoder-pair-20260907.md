# T007-B Stable Text Decoder Pair — 验收执行证据（2026-09-07）

Status: DONE。Selector：CPP(Spec182TokenizerStable/*)。
实现：tokenizer-bridge `lib.rs` 实现冻结的 "Stable Prefix Algorithms"
（native-token-stream-design.md）——第五私有 ABI `ndi_token_decode_stable`
在 T007-A 已由 tokenizer-abi.h 声明、T007-B 由 Rust 侧 `stable_text` 分派
实现：Null/WordPiece 非终态即整前缀 HF decode（不可被后续改写）、
ByteFallback 非终态跳过过滤后 HOLD 尾部 byte-run、仅对 closed part decode、
ByteLevel 非终态按 canonical GPT-2 alphabet 映射为字节流后做 Rust-std
UTF-8 前缀 walk（完整段提交、每个 determined-invalid 单元一个 U+FFFD、
HOLD 尾部 incomplete），三 profile 的 final=true 均为完整 HF decode
（final/full 一致）；decoder pipeline 未知（Fuse 等）时 stable 调用
fail closed（含空 ids），不猜 prefix stability；C++ 侧 factory 类型名收口
`GenerationTextDecodersFactory` → `GenerationDecodersFactory`（与
`NativeGenerationTextDecoders` 对称），paired factory 与 handler factory
声明进入 NativeProviderHandlerConfig，生产注入由 T009-C/T011-B 接线。
验收数据为冻结 stable-vectors.json（5 fixtures / 28 rows：
byte-fallback-special 11、bytelevel 13、reject-fuse 0、legacy-ascii 2、
legacy-unicode 2），经真实静态链接 native ABI 逐 cut 对照。

## 执行命令与结果

```
./waf -o build-nac182 configure --prefix=.../.codex-tmp/spec182-t006c-l0/staging \
    --with-tests --nac-abe-prefix=.../nac-abe-integration-182/install \
    --onnx-prefix=.../.codex-tmp/spec182-t001-dependencies/onnx-install \
    --ndn-svs-source-tree=/home/tianxing/NDN/ndn-svs \
    --ndn-svs-build-tree=/home/tianxing/NDN/ndn-svs/build
#   rc=0；configure 的 _ensure_tokenizer_bridge stale() 检测到 lib.rs 新于
#   archive，重跑 cargo（--locked --offline -j2 独立 target 目录）
./waf -o build-nac182 build -j4        # rc=0；cargo 产物 13:03:24 重建
#   陷阱：waf content-hash 不签名外部 STLIB 内容——archive 变化不触发
#   relink，17s build 无任何产品更新；touch 源码无效（content sig 不变）。
#   修复 = 显式删除旧 link products 后重链：
rm -f build-nac182/libndnsf-distributed-inference.so build-nac182/unit-tests
./waf -o build-nac182 build -j4        # rc=0；.so 13:06:12 / unit-tests 13:06:25 relink
./build-nac182/unit-tests --run_test='Spec182TokenizerStable/*'
#   rc=0；9/9 全绿（frozen list/digests、三 profile 前缀对照、final==full、
#   空 ids、交错无状态、Fuse fail-closed、未知/越界 id）
./build-nac182/unit-tests --run_test='Spec182TokenizerFull/*'
#   rc=0；7/7 全绿（T007-A 回归不受 stable 路径影响）
./build-nac182/unit-tests --run_test='Spec182NativeTokenizer/*'
#   rc=0；3/3 全绿
./build-nac182/unit-tests --run_test='!StreamFacade'   # 完整回归，No errors detected
#   rc=0（.codex-tmp/t007b-full-regression.log）
#   注：裸跑仍见环境性 StreamFacade 族 PredictiveProviderExactWireValidation-
#   AndAtomicFlush 段错误 rc=139，与 T006-B/C/D/T007-A 同族，负结果已记
#   failure-log 2026-09-07；回归 gate 固定排除该族
```

raw 输出见 `.codex-tmp/t007b-full-regression.log`；T007-A 的 configure/
build log（`.codex-tmp/t007-configure-r2.log`、`t007-build-r3.log`）与本卡同
一 build dir 复用；代理级逐 byte 探针 raw 见 `/tmp/t007b-abi-probe/`、
`/tmp/t007b-surrogate-check/`（rustc micro-test）。

## 卡 Steps 对照

- **新增第六私有 stable ABI、同摘要 paired factory 和 handler factory
  declarations**：`ndi_token_decode_stable`（tokenizer-abi.h，T007-A 已
  声明）与 `NativeTokenizer::decodeStable(ids, skip, final)`（T007-A 已
  落地，skip 默认 true）构成第六 ABI 的 C++ 侧；本卡把 `stable_text`
  五参变体在 Rust 侧实现（Profile 分派 + final 语义），并把
  `GenerationTextDecodersFactory` 收口为 `GenerationDecodersFactory`
  （NativeProviderHandler.hpp/.cpp、examples/DI_NativeProviderExecutable.cpp），
  与 T007-B 卡声明的 paired-factory 接入点对齐——生产注入留 T009-C/
  T011-B。
- **分别实现 frozen ByteLevel/ByteFallback profiles 和 final flush**：
  `lib.rs` `Owner::stable_text`——ByteFallback 非终态先做 skip 过滤
  （special 内容以 added-first 优先级解析 content 判定，skipped specials
  消失、retained specials 关闭 run），再持尾部 byte-run 并 decode closed
  part（`is_byte_token` 六字节 `<0xNN>` 形状与 HF ByteFallback decoder 同
  判）；ByteLevel 非终态全 token all-or-nothing alphabet 映射后走
  `stable_prefix`（`std::str::from_utf8` error 归因逐字节实现，见
  failure-log 的 std-vs-python surrogate 分歧修正）；三 profile
  final=true 一律 `self.decode`（与完整 decode 路径同结果同限制），
  Null/WordPiece 非终态即整前缀 decode；Unsupported（含空 ids）返回
  "unsupported decoder profile for stable text"。Cargo.toml/Cargo.lock 零
  变化，crate 仍 pin 0.20.3/Rust 1.90.0 独立 target 目录。
- **生产注入由 T009-C/T011-B 完成**：本卡未注入任何生产调用点；handler
  侧仅有类型名收口，运行语义不变（既有回归全绿证明）。

## 卡 Verify 对照

- **CPP(Spec182TokenizerStable/*)**：9/9 全绿，gate 在
  `tests/unit-tests/distributed-inference-tokenizer.t.cpp`（无 wscript，
  tests glob 自动拾取；case-manifest 已手术式改名该行并登记 9 个 named
  cases）。Suite 内 self-check：五个 fixture 名/profile/case 数与四条
  fixture sha256 硬编码断言 + materialize 时 sha 复验。
- **合法 U+FFFD、truncated/invalid bytes、specials、interleaved calls、
  final/full 一致**：`legalReplacementCharacterKept`（bytelevel）与
  `legalReplacementFlushOnFinal`（byte-fallback-special）钉住合法 U+FFFD
  输入逐字节保留、绝不被算法删除；`truncatedLeadHeldAndFlushed`、
  `truncatedTwoContinuationsHeld`、`loneInvalidByteCommitted`、
  `determinedSubpartWithBoundaryBreak`、`leadByteBreakCommitsReplacement`、
  `surrogateSequenceCommits` 覆盖 truncated/invalid 归因；specials 双路
  （skip=true 消失、skip=false retained 关闭 run/映射为 ASCII 字节）；
  `InterleavedStableCallsAreStateless` 12 步交错两遍收集对比（无状态性）；
  每行 `compareStableRow` 断言 `decodeStable(ids,skip,true)==finalText` 且
  `decode(ids,skip)==finalText`（final==full）与每条 frozen prefix 都是
  finalText 的文本前缀（前缀永不改写已提交输出）。
- **拒绝未知 stream profile，不猜 prefix stability**：
  `RejectFuseFailsClosedForStableOnly` 对 Fuse decoder 的 stable 调用全部
  throw（空 ids、非空 ids、final 两态），同时断言完整 decode/encode 不受
  影响；`StableRejectsUnknownAndOutOfRangeIds` 负/超 u32 id 在 ABI 前
  invalid_argument、词表外 in-range id 为 runtime_error——绝无猜测文本。
  `EmptyIdsAreStableOnEverySupportedProfile` 四 profile × 两 skip × 两
  final 全为 ""。

## 关键发现与修正

- 冻结向量代理（`author-stable-vectors.py`）的 ByteLevel 期望曾以 python
  codec 语义推导，与 native Rust-std 语义在 surrogate 序列
  （0xED 紧约束第二字节 A0..9F）上分歧：std 对 `ED A0` 立即判 determined
  error（只消耗 lead），python codec 等待第三字节。修正为按
  `std::str::from_utf8` error 归因规则镜像的 `utf8_error`/`walk_lossy` 代理
  （tight 约束、continuation 消耗 k、尾部 None→held、final flush 单
  U+FFFD），HF 在 full length 仍作独立 cross-check；surrogate 前缀随之冻结
  为 ["", "��", "���"]。代理自身断言过 HF 全程一致性（`stream_text(raw,True)
  == tokenizer.decode(ids,skip)` 对每行）。
- waf 对 pinned 外部 staticlib 的内容不做签名跟踪：cargo 重建后（13:03:24）
  仅靠 `waf build` 不 relink（17s no-op，touch 亦无效——content hash 而非
  mtime）。修复 = 删除 .so/unit-tests 产品强制重链（13:06:12/13:06:25）；
  stale archive 曾造成 Spec182TokenizerStable 6 例假失败（旧 bridge 无
  Unsupported arm → RejectFuse 不抛），relink 后自愈。负结果与根因已记
  docs/failure-log.md 2026-09-07 T007-B 条目。
- 代理曾以 legacy 样本 encode 推导 rows：WordLevel 将 "token-1" 按标点拆成
  三个 UNK 导致退化行；改为 decode-derived 显式 vocab 名 → 每 fixture 两
  skip 态各一整行多 cut 前缀。
- ByteLevel synthetic fixture 的 decoder JSON 需要 python 0.20.3 bindings
  的显式完整字段集（{"type":"ByteLevel","add_prefix_space":true,
  "trim_offsets":true,"use_regex":true}），field-less 变体被 wrapper 拒绝；
  Fuse decoder 作为 "unsupported" 的合法可加载 pipeline（decode 非空）被选
  为 fail-closed 冻结 fixture。

## 文件

- [tokenizer-bridge/src/lib.rs](../../../NDNSF-DistributedInference/cpp/adapters/qwen/tokenizer-bridge/src/lib.rs)
  （stable_text/Profile/stable_prefix/effective/build_owner 实现）
- [tokenizer-bridge/tokenizer-abi.h](../../../NDNSF-DistributedInference/cpp/adapters/qwen/tokenizer-bridge/tokenizer-abi.h)
  （第五 ABI 声明，T007-A 起冻结）
- [NativeTokenizer.hpp](../../../NDNSF-DistributedInference/cpp/adapters/qwen/NativeTokenizer.hpp)
  （decodeStable 门面，T007-A 起冻结）
- [NativeProviderHandler.hpp/.cpp](../../../NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp)
  （GenerationDecodersFactory 类型名收口）
- [examples/DI_NativeProviderExecutable.cpp](../../../examples/DI_NativeProviderExecutable.cpp)
  （同一 factory 成员改名）
- [distributed-inference-tokenizer.t.cpp](../../../tests/unit-tests/distributed-inference-tokenizer.t.cpp)
  （Spec182TokenizerStable 9 cases）
- [author-stable-vectors.py](../../../tests/fixtures/spec182/dependency-probes/tokenizer/author-stable-vectors.py)
  （frozen 生成器；镜像 Rust-std 归因 + HF cross-check）
- [stable-vectors.json](../../../tests/fixtures/spec182/dependency-probes/tokenizer/stable-vectors.json)
  （schema spec182-tokenizer-stable-vectors-v1；whole-file sha256
  a80597b3c96833a61a4dd22606b014715f62e2111e0bd7b3325cc97665bc6254；
  fixture shas：bf 3aef42a9…9a5f、bytelevel bef4550b…01f4、reject-fuse
  cb285631…b4e3、legacy-ascii bf0f0fa6…a5a6、legacy-unicode 90db6ef1…2404）
- [case-manifest.json](../../../tests/fixtures/spec182/case-manifest.json)
  （T007-B 行改名 + 9 named cases）
- [failure-log.md](../../../docs/failure-log.md)（2026-09-07 T007-B 条目）

## 残余风险

- ByteLevel/ByteFallback 的 content 分类在 owner create 时对全部可达 id
  枚举一次（added-first 语义），若未来 added token 在运行时变化（当前
  API 无此路径）需重建 owner；stable 契约本身无状态、每次调用独立。
- Null/WordPiece 的前缀稳定性以"不可被后续改写"为前提由 HF decode
  直接承担：本卡只对两个 frozen legacy fixture 验证，其它 null/WordPiece
  pipeline 形状（分隔符/去重选项）未逐一枚举——生产侧若引入新形状需重走
  冻结流程。
- ByteLevel 的 alphabet 映射假定 canonical GPT-2 alphabet（byte 32→U+0120
  等 33 个缺位映射），byte_map 在 owner 构造时按该表生成；若未来某
  ByteLevel 词表使用非 canonical 映射，stable 路径会将其当 whole-token
  raw bytes 回退——full decode 不受影响，但 stable 前缀会变保守而非错误。
- 完整回归门排除了环境性 StreamFacade 段错误族（负结果见 failure-log）；
  focused/回归 suites 与全量（排除该族）均绿。
