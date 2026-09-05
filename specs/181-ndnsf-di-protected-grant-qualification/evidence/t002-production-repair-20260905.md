# T002 Native Production Wiring Repair

**Date**: 2026-09-05 | **Source baseline**: `778d69d9`
**Layer**: wired + executed（build / 定向 parity） | **Status**: OPEN

## First Boundary

A01/A02：native executable 未安装 protected factory，装配路径长期缓存
明文模型。此次接入注册表/精确获取/真实 runtime，向 preparation factory
传递同一 runtime，受保护装配只保留密文缓存；私有临时目录在任何
明文写入前登记，加载从落盘密文认证解密并受同一清理责任管理。
普通无 execution lease 的请求使用 sealed Selection 与 Provider boot
派生的本地 attempt fence；有 execution lease 时继续使用其已有 fence。
这不签发或替代 execution lease 能力。

## Development Checks

存储/目录所有权的初步定向 C++ 检查为 13 cases PASS，包括已有 11
runtime 用例及 2 个存储/目录用例。日志在忽略的工作区临时根目录
`spec181-t002-production-20260905-r1/{storage-build,storage-test}.log`。
后续存储单元已在 `571fc042` 提交；20 项 C++、55 项 Python/跨语言
存储定向检查通过，见 [storage repair](t002-storage-repair-20260905.md)。
这不是 Provider 网络集成结果，T007 仍为 BLOCK。

## Build Attempts

`SPEC181-T002-PRODUCTION-R1`：2026-09-05，baseline `778d69d9` 加本记录
所列 working-tree wiring。`python3 scripts/spec180_native_build.py build
--build-dir build-system-j2` 在 C++ 编译阶段退出 1：
`NativeProtectedProvider.cpp:170: DelegationList is not a member of ndn`。
当前安装的 ndn-cxx `Interest::setForwardingHint` 接受 `std::vector<Name>`。
原始日志保留于忽略的工作区临时根目录
`spec181-t002-production-20260905-r1/native-build.log`；这是编译失败，
没有产生网络或保护执行结果。按当前本地头文件修正后，新尝试使用 r2。

`SPEC181-T002-PRODUCTION-R2`：同一命令在新目录记录，修正转发提示
接口后退出 0，输出 `SPEC180_NATIVE_IDENTITY_OK`。实际编译链接
framework/native executable，并强制重建 Python extension；加载库与
源码/构建配置身份验证通过。日志位于忽略的工作区临时根目录
`spec181-t002-production-20260905-r2/native-build.log`。
`build-system-j2/spec180-native-build.json` 的 SHA-256 为
`29cfe34f0e818d7c003e34018585d7eba68b0b1a718c35dd9cfbd28d68ce5cf7`。
该文件绑定实际 working-tree 源码，不能把 HEAD 单独当作构建源码。

重建后运行 `python3 -m pytest -q
tests/python/test_spec181_native_grant_parity.py`，3 tests PASS，覆盖既有
9 grant vectors，日志为同一 r2 的 `rebuilt-grant-parity.log`。
没有执行完整 suite、MiniNDN、SIF 或 Tiger。编译故障已关闭；真实
网络、ORT 生命周期及全部资源上界仍开放。

## Change Boundary Details

`NativeProviderHandler.*`、native executable、wscript 及 Core-flow 测试
均有此前工作，保留其边界；本轮按修复差异单独核对，不整体提交
无关旧修改。`NativeCanonicalOnnxAssembler.*` 原为已有未跟踪实现，
其保护分支属于此次生产接线，但整体采用仍须通过定向验证。
未提交生产文件为 `NativeProtectedProvider.*`、
`NativeCanonicalOnnxAssembler.*`、`NativeProviderHandler.*`、
`examples/DI_NativeProviderExecutable.cpp`、`examples/wscript`、
`tests/wscript`、`tests/integration-tests/ndnsf-di-core-flow.t.cpp`。
其中后六组及 assembler 包含此前实现，不能以本次 build 代替对其
完整审查。存储单元已单独提交，生产文件保留原工作区差异。
