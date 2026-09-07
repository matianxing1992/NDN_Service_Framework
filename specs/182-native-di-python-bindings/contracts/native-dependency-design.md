# Native Dependency Design

**Status**: IN_PROGRESS / T001; ONNX dependency feasibility PASS; tokenizer ABI feasibility PASS
**Authority**: [code design](code-design.md), [proof](proof-design.md)

## Scope and Evidence

用户已授权在Experimental完成182；revision7的“本轮仅审计”是前一轮范围，不阻止本轮T001设计关闭与必要依赖可行性探针。产品实现仍须先关闭相应设计门。探针只证明明确的依赖API/字节能力，不计T006/T007实现或T016运行资格；不启动SIF/Tiger。

## ONNX Candidate

首选官方ONNX 1.17.0 C++库（与冻结assembly-vectors版本相同），复用其ModelProto、checker和shape inference。候选protobuf为本机现有3.6.1，protoc与libprotobuf必须同源；这不是宣布与Python protobuf 5.29.6 ABI相容，而是比较独立生成的确定性wire。若输出不相等，先定位字段/算法/编码差异；备选为同源锁定新版protobuf，最多两个候选，不重写oracle。

源：[ONNX v1.17.0 CMake](https://github.com/onnx/onnx/blob/v1.17.0/CMakeLists.txt)。构建BUILD_ONNX_PYTHON=OFF，保留checker/schema与full protobuf；构建期Python生成工具允许。全部源/构建/安装放独立`.codex-tmp/spec182-t001-dependencies/`，system GCC/binutils、最大-j2，禁止使用宿主Python扩展充当C++库。

### Bounded Probe Contract

ADD `tests/fixtures/spec182/dependency-probes/onnx-probe.cpp`：测试专用程序，输入一个Spec181固定向量JSON文件，逐个读取accepted=true的canonicalModelHex/initializerHex与recipe输入输出；输出每例byteEqual及总计，任一不等或异常exit1。不读取新assembler产出的expected。

- `decodeHex(text) -> string`：只接受偶数合法hex；owned bytes，上限由fixture小模型限制，错误抛异常。
- `inlineExternal(model, bytes)`：仅把fixture已有external initializer的有界offset/length拷贝到raw_data，恢复DEFAULT并清除external_data；不模拟生产认证或安全路径，生产路径仍由CD-005负责。
- `extract(source, inputNames, outputNames) -> ModelProto`：官方shape inference后从outputs逆向搜集、inputs截断、按原node顺序复制；按原initializer/value_info顺序保留被引用值；输入输出按recipe顺序；模型meta匹配1.17.0 Extractor。此次fixture无local function/sparse initializer，探针不得宣称覆盖这些生产迁移义务。
- `deterministicWire(model) -> string`：官方protobuf CodedOutputStream的确定性编码；先checker(full_check)再编码。没有缓存、网络、Python调用或DI业务owner。
- `main(argc, argv)`：恰好一个fixture路径，限制四个已冻结正向case；对照原expected.modelHex，输出差异的首byte和长度。范围含inline-range/component、external-component和symbolic-range；错误路径不算密码/recipe验证PASS。

源和probe需先静态审查（拓扑顺序、IO/metadata、external offset边界、独立expected），再编译执行。保留configure/build/probe命令与退出码、源码revision、库hash和`ldd`；构建上限1800秒，超时保存首边界，不自动整树重编。

2026-09-06 source review：probe按固定集合要求四例齐全且不重复；input边界截断、原节点/initializer/value_info顺序、IO顺序和producer/IR/opset按1.17.0 Extractor处理。external offset/length禁止符号和越界、所有字节owned；coded stream析构完成后才返回bytes。expected只来自未修改的Spec181向量，任一byte不等exit1。此读码审查允许执行本依赖探针，不是产品T015 PASS。

configure/build/probe-build/probe R1均exit0：ONNX源码`b8baa8446686496da4cc8fda09f2b6fe65c2a02c`，system GCC9.4/binutils2.34，protobuf/protoc3.6.1，BUILD_ONNX_PYTHON=OFF，CMake cache核对通过。raw root为`.codex-tmp/spec182-t001-dependencies/onnx-r1/`，含configure.log、build.log、probe-build.log、probe.log。

| Case | Byte comparison | Bytes |
| --- | --- | --- |
| inline-range | PASS / exact | 199 |
| inline-component | PASS / exact | 238 |
| external-component | PASS / exact | 242 |
| symbolic-range | PASS / exact | 214 |

探针输出`ONNX_DEPENDENCY_PROBE PASS cases=4`；ldd只包含system protobuf17、libstdc++、libm、libgcc、pthread、libc、zlib和loader，无libpython。此证据证明这四例的native依赖/字节可行性，不覆盖负例、canonical identity、生产保护/取消或完整CD-005；O-002剩余叶子接口与完整算法仍需冻结。

| Artifact | SHA256 |
| --- | --- |
| unchanged Spec181 assembly-vectors-v1.json | `5e035fccaa7fecc0fc5fe272a19ec52c5c7e3163f83a21405dfef9e2a6e30165` |
| onnx-probe | `3481d26994d066a849161438f9e44ef330ac4a0c8c4c1cbe98e70eee925ee046` |
| libonnx.a | `05863985fe45155f2411f9a2e862345ea60884c4a730190a6157b03e6c3c40eb` |
| libonnx_proto.a | `291f4600137192ace35cd1f049deba215a9e78bb96b15d368b157cada475f87a` |
| system libprotobuf.so | `2690b9f07a666d1bc61c9723a2ca4cad20c4ba34410f8797dc4383ff6cb2f73d` |

复现编译：`/usr/bin/g++ -B/usr/bin -std=c++17 -O1 -DONNX_ML=1 -DONNX_NAMESPACE=onnx -I<onnx-src> -I<onnx-build> tests/fixtures/spec182/dependency-probes/onnx-probe.cpp <onnx-build>/libonnx.a <onnx-build>/libonnx_proto.a /usr/lib/x86_64-linux-gnu/libprotobuf.so -pthread -o <run>/onnx-probe`；执行`timeout --kill-after=5s 30s <run>/onnx-probe tests/fixtures/spec181/assembly-vectors-v1.json`。尖括号路径对应上述本地root，不把开发机绝对路径当成可迁移依赖锁。

## Tokenizer Candidate

现有`StandaloneQwenTokenizer`使用tokenizers 0.20.3，encode的add_special_tokens和decode的skip_special_tokens均可配置、默认true。现有CD-006无此两参数，必须在冻结ABI前修正，不能退化为只支持一种行为。

首选使用同版本Rust tokenizers，通过项目自有小型C ABI适配器供C++调用：不是新的tokenizer算法。固定crate版本及Cargo.lock，default-features=false、onig；关闭http，运行时不下载资源。返回owned字节/IDs并提供同一allocator的free函数；错误和panic不能跨ABI，const模型handle只读且C++串行保护，销毁在所有调用结束后。

构建工具链候选固定Rust1.90.0 x86_64-unknown-linux-gnu；官方dist归档先核对SHA256，安装到本探针目录的rust-prefix，不修改shell profile或系统工具链。Cargo registry/target放本探针目录，最大-j2；未来生产可迁移构建需同时锁定Cargo.lock与源crate校验和，不把下载工具链本身记成ABI探针PASS。

对比候选[mlc-ai/tokenizers-cpp](https://github.com/mlc-ai/tokenizers-cpp/blob/main/include/tokenizers_cpp.h)：当前接口没有special-token布尔参数，且其crate选0.21.2，与现有0.20.3不同。直接采用会改变契约；不以更新reference版本消除差异。官方版本依据：[tokenizers 0.20.3](https://github.com/huggingface/tokenizers/blob/v0.20.3/tokenizers/Cargo.toml)。

O-003 CLOSED：下文已固定C ABI参数/缓冲释放/错误结构、Rust工具链、crate锁、生产路径和串行寿命，并通过ASCII/Unicode/special/byte-fallback的原版本独立向量对照。此关闭限于依赖/ABI设计；T007产品实现和T016运行资格仍未完成。

### Tokenizer ABI Probe Contract

ADD `tests/fixtures/spec182/dependency-probes/tokenizer/Cargo.toml`、`src/lib.rs`及同目录`tokenizer-abi.h`、`probe.cpp`：T001测试专用静态库和C++consumer，不链接到产品，T007复用前必须按正式CD-006目录/资源控制完成迁移。

ADD 同目录`generate-vectors.py`、`vectors.json`与`Cargo.lock`。generator只在离线冻结oracle时运行，严格要求旧Python tokenizers0.20.3；读取Spec175原有ASCII/Unicode tokenizer并补充含NFC、ByteFallback和special post-processing的小型BPE。每个fixture覆盖7种文本×2种encode flag×2种decode flag，记录原始tokenizer JSON、SHA256、IDs和decoded文本，共84例。C++consumer只读冻结JSON，要求三种fixture各28例，并执行14个非法ID/flag/UTF-8/null输入检查；任何差异退出非零。生成器和Python均不是native consumer的运行依赖。

`NdiTokenResult { uint8_t* data; size_t size; int32_t code; }`为repr(C)。code=0成功、1输入/backend错误、2捕获panic；成功encode的data是little-endian uint32 token IDs，size为字节数且为4的倍数；decode成功是UTF-8字节，失败data是UTF-8错误，所有buffer由同库分配。空buffer为nullptr/0。结果非copy-owning对象；C++RAII持有并只释放一次。

| Function | Parameters / result / ownership |
| --- | --- |
| `ndi_token_create(const uint8_t* json,size_t size,void** handle)` | json是已读入的owned tokenizer bytes，调用期间borrow；1..32MiB。handle非空且先置null；成功Box<Tokenizer>作为opaque owner转交，错误不遗留handle。生产摘要校验在C++adapter，本probe只核对固定工件 |
| `ndi_token_encode(const void* handle,const uint8_t* text,size_t size,uint8_t addSpecial)` | handle必须存活；text严格UTF-8且≤1MiB，0长度可空；flag只允许0/1，返回owned LE IDs；编码数量≤1Mi tokens |
| `ndi_token_decode(const void* handle,const uint32_t* ids,size_t count,uint8_t skipSpecial)` | ids调用期间borrow，count≤1Mi；flag只允许0/1，所有ID须存在于vocab；输出UTF-8≤16MiB，返回owned buffer |
| `ndi_token_free(uint8_t* data,size_t size)` | 仅对同一result的原始data/size调用一次，null/0允许；不能传Python/C++allocator内存；用Box<[u8]>恢复并释放 |
| `ndi_token_destroy(void* handle)` | 只销毁本库create返回的owner，nullptr允许；在所有调用完成后执行，不抛异常 |

create/encode/decode的所有Rust错误和panic转换为result，不越过ABI。销毁/释放仅允许可信C++RAII传回原指针，不把ABI当成任意指针验证器。Tokenizer创建后只读，C++owner串行调用；fixture使用同一handle重复encode/decode，最终销毁。探针对照原tokenizers0.20.3生成的冻结ids/text，含special两值、Unicode、ByteFallback及非法ID/flag；不在C++consumer中调用Python。Cargo精确锁0.20.3/default-features=false/onig及生成的Cargo.lock，构建网络只取crate，不允许运行tokenizer时HTTP加载。

Rust ABI source review：repr(C)字段顺序与C头一致；Box<[u8]>与free恢复长度配对，无跨allocator释放；create预置空handle，错误不发布owner；borrowed对空/size限额校验、u32对齐由C++vector保证；UTF-8、flags、未知ID分别拒绝，panic返回code2。probe单线程复用handle，未接入生产，允许本单元依赖构建和ABI对照；资源耗尽导致进程abort不声称能转换为可恢复错误。

### Tokenizer Probe Result

2026-09-06 R1：`cargo build --release --locked -j2`、system GCC C++consumer编译及30秒有界运行均exit0。Cargo.lock锁73个package（含probe本身），核心tokenizers=0.20.3；Rust1.90.0。`legacy-ascii`、`legacy-unicode`和`byte-fallback-special`各28例精确一致，输出`TOKENIZER_ABI_PROBE PASS cases=84 negatives=14`。ldd仅system libdl/pthread/stdC++/m/gcc/libc/loader，无libpython。原始lock/build/probe-build/probe日志位于`.codex-tmp/spec182-t001-dependencies/tokenizer-r1/`。

| Artifact | SHA256 |
| --- | --- |
| tokenizer-probe | `e02031b99c657fa6ae5e60c831439fca1ba5b076348b51cc68a1aac3ef26079a` |
| libspec182_tokenizer_probe.a | `b0469e32c3f82a6c892caf8036a3b404d095d388078254730f75a157637ae980` |
| Cargo.lock | `a9352585ceb07cdc8e1fe798ca6e222d10791d89d7aa3ed8ecb3b5a5172f71f1` |
| vectors.json | `6f44a2e62bebccd4dee2ff214d4d65f99528899e6863705eb1ec431b098ed34b` |

复现链接：`/usr/bin/g++ -B/usr/bin -std=c++17 -O1 tests/fixtures/spec182/dependency-probes/tokenizer/probe.cpp <target>/release/libspec182_tokenizer_probe.a -ldl -lpthread -lm -o <run>/tokenizer-probe`。执行`timeout --kill-after=5s 30s <run>/tokenizer-probe tests/fixtures/spec182/dependency-probes/tokenizer/vectors.json`。Rust静态库、C++consumer及冻结expected相互独立；generator只产生reference，运行不调用它。该结果不覆盖产品digest/取消、并发销毁或T014隔离器，因此不计PO-006/SC-005完成。

### Frozen Production Integration

T007新增`NDNSF-DistributedInference/cpp/adapters/qwen/tokenizer-bridge/{Cargo.toml,Cargo.lock,src/lib.rs,tokenizer-abi.h}`，保留上表五个C ABI函数、结果结构和所有权。crate命名`ndnsf-tokenizer-bridge`、静态库`ndnsf_tokenizer_bridge`，迁移probe代码前按CD-006复审，依赖版本/features及registry校验和不变。package重命名只改变lock中本地package项，不能重新解析/升级依赖。许可证：tokenizers Apache-2.0，完整传递依赖及各自license随源码交付保留。

T002在DI构建配置中声明Rust compiler/Cargo及该静态库输入；T007接入实际构建。Cargo使用独立target目录、`--locked`、至多-j2；C ABI是DI库的私有实现，桥接header/Rust crate不作为应用public API安装。静态库链接进安装的DI shared library，T016验证其真实依赖；应用只安装/包含NativeTokenizer.hpp。不得把测试probe库安装成产品库。

`NativeTokenizer`用不可复制的`unique_ptr<Impl>`隐藏Rust类型；Impl拥有`unique_ptr<void, ndi_token_destroy>`的handle、实际artifact digest与mutable mutex。constructor先有界读取同一份tokenizer字节、核对expected SHA256，再create；不存在文件、非法digest/config均不发布实例。encode/decode持mutex覆盖ABI调用及复制结果，用Result RAII释放buffer后解锁；vector<int64_t>逐项校验0..UINT32_MAX再转u32，返回ID提升到int64_t。flag按上表传递，UTF-8和大小限制不绕过。ABI code1映射adapter输入/backend错误、code2映射backend内部错误，均不产出成功文本。析构要求调用者共享所有权已保证无在途方法（并发对象销毁本身不合法）；decoder factory捕获shared_ptr保住实例，不在每token创建backend。T007必须保留digest不匹配、oversized、未知/负ID、同一owner重复调用与并发串行化unit检查。

ONNX许可证Apache-2.0，protobuf发行包license及NOTICE随输入交付；ONNX1.17.0/protobuf3.6.1探针组合已验证。精确lock见[native dependencies](native-dependencies.json)。ONNX完整canonical identity与recipe算法仍属O-002/O-004待冻结，不能用四个依赖向量代替全产品语义。

Rust download R1 exit1在TLS读取阶段报`DECRYPTION_FAILED_OR_BAD_RECORD_MAC`，部分归档不可解压/安装。raw `.codex-tmp/spec182-t001-dependencies/rust-r1/boundary.json`；保留部分输入，新R2目录有限重试并核对官方SHA256。该失败不属于tokenizer/ABI结果。
R2最小组件下载同样在Python3.8 urllib TLS读取失败，未安装；R3使用Node22 HTTPS传输作一次有界重试，仍核对官方SHA256，旧部分文件不覆盖。
R3下载exit0，三个官方组件SHA256均核验：rustc `48c2a42de9e92fcae8c24568f5fe40d5734696a6f80e83cc6d46eef1a78f13c9`，cargo `9853db03d68578a30972e2755c89c66aec035fec641cf8f3a7117c81eec2578d`，rust-std `663f4ab7945b392d5e5294dec1b050a66820a20e86f084ec37eeb0f2f7ff5569`。传输问题已恢复；只安装到独立prefix，不更改全局工具链。
