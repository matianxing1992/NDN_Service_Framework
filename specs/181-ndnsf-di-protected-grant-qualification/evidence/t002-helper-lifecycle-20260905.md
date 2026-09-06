# T002 Native Assembly Helper Lifecycle

**Status**: PASS (focused helper repair); T002 full acceptance IN_PROGRESS
**Layer**: focused production process supervision

## First Boundary

当前 helper 使用阻塞 `waitpid`；装配期间没有请求截止、取消或 grant
过期检查，且 native 读取 helper 模型文件无大小上界。R1 新增 6 个
定向回归均失败（12.24 s）：helper timeout、请求 deadline、真实 grant
取消/过期都未及时停止；已过期请求仍启动 helper；超出 role envelope
的输出仍被激活。原始结果位于 ignored workspace temporary directory
下 `spec181-t002-helper-20260905-r1/red.log`。

测试使用真实 C++ 装配入口与实际子进程。慢 helper 在暂停后 exec
正常生产 helper；protected 分支通过临时真实签名 grant 和原生解包
取得权限。文件/grant transport 使用 fixture ports，因此不构成网络
验收。私钥仅为临时生成的测试输入，不进入持久文档或 Git。

## Repair Plan

把 helper 作为受管资源注册在 staging lease 之后，利用逆序清理先
终止并回收 helper，再删除明文目录。循环检查请求/操作期限和当前
grant 权限；限制单个输出文件及 native 读取大小，失败不得激活缓存。
T002 完整生产验收仍开放，T007 BLOCK。

## Runtime Loader Repair

R2 编译通过，但 14 个 native 检查在装配前失败：可执行文件从
`/usr/local/lib` 加载旧 framework，缺少 `streamCancelled` 符号。
`ldd` 与 `readelf` 确认目标只有 SVS RUNPATH；当前 build 的 framework
确实导出所需符号。R3 给 Waf target 增加当前 build RUNPATH，并让
测试环境把该目录置于动态库路径首位。此前 T003 的 file-backed
装配检查没有调用这个 Core API；本轮重新在当前 framework 上验证
完整 parity，不能把 loader 失败计作装配或授权拒绝。

R3 原始 6 个回归和 parity 共 25 PASS，增加外部线程调用 runtime
取消后为 26 PASS。统一 native 构建也完成。但在 source fetch 返回
的确定边界取消，新增回归实际发现 `canonical.onnx` 留存：runtime
已删除 staging，后续父进程写入却重新创建目录。失败保留在 r3
`fetch-cancel-red.log`；R4 把所有受保护 staging 写入与权限检查放在
同一 runtime 锁内，解密后的明文写入也纳入同一临界区。

## Focused Acceptance

R4 `final-focused.log` 为 27 PASS：8 个真实子进程/取消边界检查，
16 个固定装配向量双入口检查，3 个 grant parity 检查。超时、请求
过期、回调取消、其他线程取消和 grant 过期在 1.2 s 断言上界内
结束，均不等到 helper 的 1.5 s 慢操作完成；已过期请求不启动
helper。返回时 helper PID（包括 zombie）不存在，runtime 清理后
state 为 Zeroized；source-fetch 取消不再重建明文目录。

```bash
./waf -o build-system-j2 build -j2 --targets=spec181-assembly-parity
env SPEC181_ASSEMBLY_PARITY_BINARY="$PWD/build-system-j2/spec181-assembly-parity" \
  PYTHONPATH=NDNSF-DistributedInference:pythonWrapper:NDNSF-DistributedRepo/pythonWrapper \
  python3 -m pytest tests/python/test_spec181_native_assembly_lifecycle.py \
  tests/python/test_spec181_assembly_parity.py \
  tests/python/test_spec181_native_grant_parity.py -q
```

helper 独占进程组；先终止该组并 waitpid 回收 leader，再释放 PID
身份。它的 runtime lease 晚于 staging 注册，因此逆序清理先处理
helper。正常结束仍复核截止时间、权限和输出上界；受保护源文件、
initializer、helper 请求及最终明文的写入与 runtime 清理使用同一锁。
模型 native 读取不超过 role `maxAssembledBytes`，manifest/结果/log
最多 65536 bytes；helper 的文件大小还受 RLIMIT_FSIZE 约束。

本轮同时将既有 `NativeCanonicalOnnxAssembler.{hpp,cpp}` 和正常
`native_assembly_helper.py` 纳入源码检查点；生产 factory/handler 的
其余工作区变更继续归 T002。R3 统一 native 构建已通过，但 R4 的
目录竞态修复需要再次重建；新的受保护 Y-B 定向控制已准备独立 run
目录，尚未执行。T002 不能勾选，T007 仍 BLOCK。
