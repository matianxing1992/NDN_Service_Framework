# T006-C Bounded Native Worker — 验收执行证据（2026-09-07）

Status: DONE。Selector：CPP(Spec182OnnxWorkerProtocol/*)。承载 suite 建于
[tests/unit-tests/di-native-onnx-recipe.t.cpp](../../../tests/unit-tests/di-native-onnx-recipe.t.cpp)
（execution-units.md 的 U/I 行与 manifest 差异按 registry 约定以 planned 文件
di-native-onnx-recipe.t.cpp 为承载，沿用 T006-B 决策）。实现：固定匿名 pipe
请求/响应协议 + 限额/partial-frame 状态机 + deadline/poll/kill/reap transport，
全部置于
[NDNSF-DistributedInference/cpp/adapters/onnx/](../../../NDNSF-DistributedInference/cpp/adapters/onnx/)
（NativeOnnxAssemblyWorker.{hpp,cpp} 新文件 + NativeOnnxRecipeAssembler.cpp
native worker 构建路径）；main 只调库，无路径命令入口。
[examples/DI_NativeOnnxAssemblyWorker.cpp](../../../examples/DI_NativeOnnxAssemblyWorker.cpp)
为 L0 安装目标（install_path='${LIBEXECDIR}/ndnsf-di'，随 staged install 落盘）。

## 执行命令与结果

```
./waf -o build-nac182 build -j4    # rc=0（升级后 12GB/6-core；17m55s 一次干净全量）
./waf -o build-nac182 install -j4  # rc=0（3m5s；仅 NDNSF-DistributedRepo pythonWrapper
#   dev-pip -e install 失败 rc=1 -> "continuing staged install"，wscript 设计内非致命路径：
#   NDNSF-DistributedRepo/wscript:31-34 捕获 CalledProcessError 后继续）
./build-nac182/unit-tests --run_test=Spec182OnnxWorkerProtocol --log_level=test_suite
#   rc=0；27 个 case 全绿，*** No errors detected
./build-nac182/unit-tests --run_test='!StreamFacade'   # 完整回归 842 cases，No errors detected
```

raw 输出见 `.codex-tmp/t006c-*.log`（reconfigure/objbuild/workerbin/providerbin/
suite-build/suite-build2/suite-run-final/full-regress/full-regress-minus-streamfacade）。

## 卡 Steps 对照

- **固定匿名 pipe 协议**：request = 8B magic `NDI182A1` + u64le metadataLength +
  u64le modelLength + u64le initializerLength + u8 hasInitializer（flag 0 ->
  initLen 必须为 0）+ metadata + model + initializer；response = 8B `NDI182R1`
  + u32le metadataLength + u64le modelLength + u8 status（0 ok / 1
  algo-reject / 2 protocol-input-error）+ metadata + model（仅 status 0）。
  子进程 exit code 0/1/2 与 status 语义一致；stderr diag 仅
  `DI_NATIVE_ONNX_WORKER: <msg ≤400B>`。codec/解析器位于 worker cpp
  （decoders/compose/finalize 区段）。
- **限额/partial-frame 状态**：Request/ResponseFrameDecoder 逐字节状态机，
  任何前缀截断返回 needs-more；header 内长度与 8B magic 常量、status/
  hasInitializer 一致性、frame 内 total 大小与元数据长度一致性、重复
  second-frame 一律拒绝；限额由本卡调用方传入（超限 -> PROTOCOL-INPUT-ERROR
  语义，suite 以 oversize 行覆盖）。parseJson 为完整递归 descent parser
  （拒绝 duplicate keys）。
- **deadline/poll/kill/waitpid**：transport runAt 锚定 stdout Eof；
  killAndReap 走 TERM -> 100ms×10 poll -> SIGKILL -> blocking reap；取消语义
  分 cancel-before-start 与 cancel-during-blocking-worker（后者实测 escalation
  1304ms 完成，case 覆盖）；子进程被信号杀死、静默退出、stdout 垃圾字节均
  有独立 case。
- **main 只调库，无路径命令入口**：[examples/DI_NativeOnnxAssemblyWorker.cpp](../../../examples/DI_NativeOnnxAssemblyWorker.cpp)
  由 wscript 生成含 worker transport 的 L0 二进制；无任何 argv 路径命令面。
  父进程注册 worker 位置经 register 流程：empty path / open fail / empty
  computed / non-empty sha mismatch -> PREFLIGHT 失败族（case 覆盖）。

## 关键发现与修正

1. **isSha256Digest 长度门 66 != 71（实现 bug，被 suite 捕获）**：
   `"sha256:"`(7) + 64 hex = 71 bytes，代码误用 66，导致 worker 中每个合法
   digest（envelope recipeDigest、certified slice digest 字段、result
   modelDigest）都被拒 -> validate 恒返回 METADATA "request metadata
   recipeDigest is invalid" 并丢弃已解析 slice。修复为 `size()==71` + 注释。
   此单一 root cause 解释三个 failing cases 的症状簇。详见
   [docs/failure-log.md](../../../docs/failure-log.md) 2026-09-07 两条记录。
2. **waf 内容签名再签名与闭包漂移**：wscript use= 变更触发 635-task
   re-signature 全量重编译；spec181 时代 target 只 use ONNX，激活的
   ONNXRUNTIME 宏需要 onnxruntime include -> 全量 rebuild 失败。修复：tests
   wscript 的 spec181-assembly-parity use= 补上 ONNXRUNTIME。教训（长度常量
   与闭包漂移）均已写入 failure-log。
3. **升级后机器全量验证**：机器升级 12GB/6-core 后 build -j4 / install -j4
   均 rc=0；staged install 的 pythonWrapper dev-pip 消息为
   NDNSF-DistributedRepo/wscript 设计内 "continuing staged install" 路径，
   非本卡缺陷。
4. **完整回归 842 cases（排除 StreamFacade）零失败**：排除的 StreamFacade/
   sign 系为 pre-existing 环境性失败族（TPM-PIB missing private key + NFD，
   T006-B 已记录；部分 hard crash 会 core 整个进程，无法用 Boost 捕获）。

## L0 安装链接验证（staging）

configure 已 pin `--prefix=$PWD/.codex-tmp/spec182-t006c-l0/staging`；
install 后验证：

```
${staging}/libexec/ndnsf-di/DI_NativeOnnxAssemblyWorker
#   -rwxr-xr-x，12,634,952 bytes，ELF 64-bit LSB executable
#   ldd：0 not found；libonnxruntime.so.1 -> /opt/onnxruntime/lib
#   （同目录 staging siblings bin/lib/include 由 T002A L0 pattern 建立）
```

真实子进程的 L0 启动/cleanup 验证按卡约定延至 T016。

## 文件

- [NativeOnnxAssemblyWorker.hpp](../../../NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp)（new）
- [NativeOnnxAssemblyWorker.cpp](../../../NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.cpp)（new：protocol codecs / transport / register）
- [NativeOnnxRecipeAssembler.cpp](../../../NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.cpp)（native worker 构建/校验路径）
- [examples/DI_NativeOnnxAssemblyWorker.cpp](../../../examples/DI_NativeOnnxAssemblyWorker.cpp)（new，L0 安装目标）
- [examples/wscript](../../../examples/wscript)、[tests/wscript](../../../tests/wscript)（worker program + spec181-assembly-parity ONNXRUNTIME 闭包修复）
- [di-native-onnx-recipe.t.cpp](../../../tests/unit-tests/di-native-onnx-recipe.t.cpp)（Spec182OnnxWorkerProtocol 27 cases）
- [docs/failure-log.md](../../../docs/failure-log.md)（2026-09-07 两条：digest 长度门、onnxruntime 闭包）

残余风险：真实子进程的 L0 启动/cleanup 收尾在 T016；decoder/state 测试
覆盖截断/溢出/重复帧与晚到结果，未覆盖超长 metadata 内容语义（限额由
调用方传入，T006-D 接续）；27 cases 中 escalation 实测依赖真实进程调度，
慢机器上 case 耗时已留足余量。
