# T003 Assembly Entry Parity

**Status**: PASS (T003 focused acceptance)
**Layer**: fixed vectors / focused production-entry integration

## Scope and First Boundary

FR-012 要求两个生产入口消费同一 canonical ONNX 与 recipe 后输出相同
字节；并未要求第二套 C++ ONNX 算法。当前 native Provider 的格式操作
通过正常子进程 helper 调用既有 certified assembler；C++ 负责获取、
摘要、请求序列化、签名和缓存激活。本次比较完整 native 入口与 Python
直接入口，不把两个 Python 函数调用冒充 native，也不声称网络/grant
验收或独立算法实现。

固定 `assembly-vectors-v1.json` 包含 inline range、inline component、
external initializer、符号维度四个正例；另有 recipe 摘要、backend ABI、
node cover 绑定和已更新传输摘要的 initializer 内容变异四个负例。
正例保留期望字节与摘要；负例要求在绑定或规范 initializer 校验拒绝。
initializer 变异重算 root/对象摘要与 recipe 绑定，以便实际到达
装配身份校验，而非只触发运输摘要检查。

`spec181-t003-assembly-20260905-r1/red.log` 位于 ignored workspace
temporary directory。首次执行 Python 8 项通过；native 8 项因缺少
本轮测试 executable 失败，首个边界是测试构建输入，不是协议拒绝。
下一步编译真实 C++ 入口和 fixture port 后使用新 run 目录验收。

R2 独立编译在 NAC-ABE 公共头失败：`nac-abe-config.hpp` 不存在。
`pkg-config --cflags libnac-abe` 表明已安装依赖需要
`-DNAC_ABE_CMAKE_BUILD`；手工编译命令遗漏该公开构建宏。失败日志保留
在 r2 `build.log`，不补造头文件；r3 使用 pkg-config 要求的宏重新编译。

R3 随后暴露手工命令还缺维护构建的 framework include 路径，NAC-ABE
嵌套头的 `common.hpp` 无法解析。停止复制构建参数，添加独立 Waf
target `spec181-assembly-parity`，直接复用现有 native 依赖与 framework
target；r4 定向编译该 target，不运行完整测试套件。

R4 编译全部通过，链接发现 framework 动态库要求 NDNSD 的
`ServiceDiscovery` 符号。新 target 的 use 列表补齐现有 `NDNSD`
依赖，r5 只重链接该 target。r4 独立再生 fixture 与固定 JSON
逐字节一致，未覆盖原始向量。

## Final Acceptance

R5 Waf target 编译/链接 PASS；`parity-with-ort.log` 为 19 PASS（6.93 s）。
其中 16 项装配检查消费固定 8 向量，3 项 grant
检查消费已有 9 向量。四个正例的 C++ 输出与固定字节、Python 输出
及 SHA-256 相同；实际 ORT CPU 对输入 3 输出 range 的 6 或
component 的 7。四个负例到达 recipe 绑定或 initializer 规范身份
拒绝，未激活模型/签名缓存，所有 staging 目录均已清理。

```bash
./waf -o build-system-j2 build -j2 --targets=spec181-assembly-parity
env SPEC181_ASSEMBLY_PARITY_BINARY="$PWD/build-system-j2/spec181-assembly-parity" \
  PYTHONPATH=NDNSF-DistributedInference:pythonWrapper:NDNSF-DistributedRepo/pythonWrapper \
  python3 -m pytest tests/python/test_spec181_assembly_parity.py \
  tests/python/test_spec181_native_grant_parity.py -q
```

固定向量包含 ONNX 1.17.0 / ORT 1.19.2 的工具版本与
`onnxruntime-cpu-v1` recipe ABI。`scripts/gen_spec181_assembly_vectors.py`
独立再生 JSON 与保存版本逐字节一致。

| Artifact | SHA-256 |
|---|---|
| assembly-vectors-v1.json | `5e035fccaa7fecc0fc5fe272a19ec52c5c7e3163f83a21405dfef9e2a6e30165` |
| spec181-assembly-parity executable | `82b090f358f3452b582bc98a81cc999ed6b10b9bc929d23f04554338f9506252` |
| NativeCanonicalOnnxAssembler.cpp | `368b283be64b5b8e971ee690d188859adef9cd9eb84d720ccc82d43bf9bc4f32` |
| native_assembly_helper.py | `8d0164f23219089986b59ff11ba12efcb411fe48fa41683b7bb663d004400eb1` |

源身份为 `e959111b` 加本轮 fixture/Waf 修改及既有 native assembler/helper
工作区实现。它们的源码提交闭包仍归 T002；这不是干净候选或同源
网络资格。测试 fetch/sign ports 使用固定本地数据，实际被测 C++
获取校验、序列化、helper 执行与缓存路径保持生产实现；签名 fixture
不构成密码学签名证明，保护授权另由 T001/T002/T006 验收。

T003 CLOSED，A07 CLOSED；T007 仍 BLOCK，下一步闭合 T001/T002。
