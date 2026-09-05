# T001 Provider Authorization and Storage Repair

**Date**: 2026-09-05 | **Source baseline**: `ae2b15a1`
**Layer**: implemented + executed（定向 unit） | **Status**: PASS（此修复单元）

## First Boundary

`SPEC181-T001-PROVIDER-R1`：聚焦 Provider 用例检验内容密钥不落盘、
从真实落盘密文加载、缺装配身份角色的独立模型绑定；原始日志在忽略的
工作区临时根目录 `spec181-t001-provider-20260905-r1/red.log`。
命令：`python3 -m pytest -q tests/python/test_spec181_provider_grant.py -k ProtectedAssemblyQualification --tb=short`，
使用仓库 DI/Core wrapper/Repo wrapper 的 PYTHONPATH。
当前失败在 Provider 存储/认证边界，不是网络或资格结果。
初始结果 2 failed / 9 passed：密钥落盘与磁盘密文变异被忽略。
独立模型绑定测试起初只断言异常家族，意外接受后续序列化异常；已把
断言收紧为 manifest 绑定错误。`red-binding.log` 保留其失败，说明原
路径并未在模型绑定处拒绝，而是在不相关的后续处理失败。

## Planned Closure

先校验 grant 与 Selection 中的规范名/摘要/身份绑定，模型预期值从
封印的 grant 名取得；解包内容密钥只登记内存租约；随后装配、加密
暂存所有 ONNX entry，从磁盘重新读取并校验密文再解密加载。
外层统一租约 context manager 覆盖全部准备、执行、取消和异常返回。

## Implemented Boundary

`_verify_protected_grant` 从 Selection 的规范名独立读取模型绑定，核对
Provider/request/attempt/core/epoch、grant 摘要及完整规范名后验证并
解包；缺 ONNX 装配身份的角色不再信任 payload 的自述 manifest。
内容密钥只进入可零化内存租约。注册 handler 在任何准备/装配前调用
该验证，整个回调由一个租约 context manager 包围，准备失败 return
与 handler 异常也会清理；装配文件在写入前登记所有权。

`_qualify_protected_assembly` 处理内联 ONNX 与单 external-data entry，
后者规范化为 `model.onnx.data`。所有 entry 分别 AEAD 加密落盘，再
从落盘字节校验上下文并认证解密；模型/权重明文登记租约。共享
canonical 文件不登记为可删除资源，测试验证源与原权重保持原样。

## Closing Tests

统一 PYTHONPATH 同 First Boundary；最终命令：
`python3 -m pytest -q tests/python/test_spec181_provider_lifecycle.py tests/python/test_spec181_provider_grant.py tests/python/test_spec181_plaintext_leases.py tests/python/test_ndnsf_di_provider_v3_boundary.py --tb=short`
→ **70 passed**。最终日志：
`spec181-t001-provider-20260905-r1/final-with-partial-write.log`。
中间 42/64/69 passed 记录分别对应 helper、注册 handler、external-data
逐步覆盖，保留各原始日志，均不作为网络资格证据。

真实注册回调的 unit 用例使用真实签名/解包/ONNX Runtime CPU 运算，
覆盖验证先于准备、错误签名阻断准备、解密后准备失败、磁盘模型/权重
密文篡改后的部分状态清理；Core delivery 与 fetch 仍是受控 fixture。
文件部分写入失败也证明其已登记 allocation 被清理。

## Remaining T001 Acceptance

注册表公钥摘要、模型策略与发布身份约束尚须接入；还须补真实 NFD
跨进程发布/精确名获取正负集成用例、资源上界及完整取消/过期消费
检查。T001 仍未勾选，本轮不关闭 T007 BLOCK 或启动完整网络资格。
