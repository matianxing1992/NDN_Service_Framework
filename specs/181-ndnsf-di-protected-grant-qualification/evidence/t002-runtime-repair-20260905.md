# T002 Native Runtime Grant Repair

**Date**: 2026-09-05 | **Source baseline**: `5b5b93a6`
**Layer**: implemented + executed（定向 unit） | **Status**: PASS（此修复单元）

## First Boundary

`SPEC181-T002-RUNTIME-R1`：已有 verifier 未接入 `ProtectedRuntime`。
新增定向 C++ 测试首先在编译处报告 `NativeProtectedGrantConfig`、
受管内容密钥接口缺失；当前不是授权或网络结果。保留日志
`spec181-t002-runtime-20260905-r1/red-build.log`，位于忽略的工作区临时根目录。
本单元仅编译实际 runtime/verifier 源码及定向 Boost 测试，不启动完整资格。

## Change Boundary

开始时 `ProtectedRuntime.hpp` 有已有未提交修改：被动序列 1、去除
撤销实现、诚实门与绑定校验声明。它们与已跟踪的 `.cpp` 及本 Spec
延期范围一致，是本修复单元所需前提；保留并在验证后纳入同一 checkpoint。
原 `distributed-inference-protected-runtime.t.cpp` 的已有修改保持未提交。

## Planned Closure

runtime 必须由真实权威验证/收件人解包取得授权，独立核对封印
grant 摘要、模型与逻辑签发者；内容密钥由 runtime 管理，过期、取消、
错误和析构清理。缺 fetch/config 时继续明确 unavailable。
Provider factory、存储 AEAD 及真实网络仍由后续 T002 工作闭合。

## Build Boundary Failure

`green-build.log`：显式 `/usr/bin/g++` 仍解析到 Linuxbrew linker，
libcrypto 的 `dlsym@GLIBC_2.2.5` 等符号链接失败；随后测试启动返回
127，因二进制未产生。二者均不是 runtime 测试结果。下一次构建使用
`-B/usr/bin` 固定 binutils，日志保留在独立 `spec181-t002-runtime-20260905-r2/`。

## Consumption Failure

r2 固定工具链后 14 项定向测试通过。随后新增实际消费与时间边界测试，
`consumption-red.log` 记录 2 用例、4 断言失败：消费 callback 抛错后仍
保留授权，fetch 耗时未计入 request deadline。下一次定向修复保留于
`spec181-t002-runtime-20260905-r3/`，不复用 r2 测试结果作为最终证据。

## Implemented Boundary

`ProtectedRuntime` 在 fetch 前比较完整封印绑定与规范 grant 名，然后
调用真实 `verifyAndUnwrapNativeGrant`，独立验证已封印 grant 摘要、
配置的逻辑权威与模型。验证后确认 `DISK_CIPHERTEXT_ASSEMBLED`
授权及 32 字节内容密钥，才进入 `GrantVerified`。

内容密钥由 runtime 独占分配，经有期限检查的 `withContentKey` 消费；
fetch 耗时计入期限，期限取 grant 和 request 的较早值。配置取消信号
在 fetch 前后和消费/数据流边界检查。callback 抛错关闭授权并清理；
错误状态析构也清理，单个 zeroizer 失败不阻断其他项，失败项保留供
显式重试。OpenSSL 临时解包缓冲和 runtime 内容密钥有擦除操作。

## Closing Tests

固定工具链的定向命令：
`/usr/bin/g++ -B/usr/bin -std=c++17 -I. tests/main.cpp tests/unit-tests/distributed-inference-protected-runtime-grant.t.cpp tests/unit-tests/distributed-inference-protected-runtime.t.cpp tests/unit-tests/distributed-inference-native-grant-verifier.t.cpp NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.cpp NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantVerifier.cpp -lcrypto -lboost_unit_test_framework -pthread -ldl -o <run-dir>/focused-runtime`
随后 `<run-dir>/focused-runtime --log_level=message`：**18 test cases，
No errors detected**。最终构建与测试日志：
`spec181-t002-runtime-20260905-r3/{build,runtime}.log`。
11 项新 runtime 用例、4 项缺配置/绑定诚实门回归、3 项 verifier 用例；
后者消费原 9 个跨语言固定 grant 字节向量。

## Remaining Acceptance

fetch 和取消信号为受控 C++ fixture，未执行 Core 网络或 native Provider
注册回调；内容密钥消费仍不是完整装配/磁盘 AEAD。T002 仍需工厂、
注册表配置、真实精确获取、受保护 ONNX 加载与全终局清理接线。
本轮定向二进制不更新统一 host build 或 Python extension，二者后续
须按当前源码重新构建。T001 资源/取消完整验收、T003 装配 parity、
T006 Provider 变异等仍开放，T007 BLOCK 不变。
