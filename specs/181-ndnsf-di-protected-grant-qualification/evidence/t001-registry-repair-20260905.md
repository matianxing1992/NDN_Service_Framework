# T001 Registry Policy Repair

**Date**: 2026-09-05 | **Source baseline**: `ed30e386`
**Layer**: implemented + executed（定向 unit） | **Status**: PASS（此修复单元）

## First Boundary

`SPEC181-T001-REGISTRY-R1`：注册表策略消费 API 和私钥公钥匹配参数缺失，
原有私钥 loader 还接受 symlink。12 项定向 unit 为 RED；不是网络结果。
命令：`python3 -m pytest -q tests/python/test_spec181_registry_policy.py --tb=short`，
使用仓库 DI/Core wrapper/Repo wrapper 的 PYTHONPATH。
原始日志在忽略的工作区临时根目录
`spec181-t001-registry-20260905-r1/red.log`。

## Planned Closure

加载冻结注册表并验证公钥摘要、算法、schema、模型族与纪元策略，
私钥必须匹配该公钥。签发者逻辑身份来自注册表，发布路由来自实际
requester 配置；限定当前 canonical manifest，保留真实网络验收缺口。

## Intermediate Failure

`green-library.log`：2 failed / 53 passed。Python 3.8 不支持
`Path.is_relative_to`，在配置路径校验处失败；改用 `relative_to` 的
异常边界兼容现有运行时后再重试，不解释为授权拒绝结果。

## Implemented Boundary

注册表 loader 校验配置状态、grant schema、Ed25519 算法、公钥文件
摘要与路径范围、模型族和保护纪元；私钥须为 mode 0600 的普通文件，
权威私钥须匹配注册表公钥。Python Provider 要求配置逻辑权威身份，
并独立比较 `policyAuthority`，不从收到的 grant 推导预期签发者。

User 使用 `deployment.user` 作为发布路由，注册表 `authorityId/keyId`
作为 payload 的权威声明；requester 与 authority 身份和密钥必须不同。
Provider 收件人查询使用实际配置前缀。签发前验证请求方身份和签名，
内容密钥按 model/epoch 归属；空模型允许列表不再代表无限授权。
允许的最终 manifest 摘要在 grant 获取时从可信 canonical binding
读取，因为 root 发布会替换初始 package 摘要；不依赖 grant view
自述值。该时序有定向回归覆盖。

## Closing Tests

统一 PYTHONPATH 同 First Boundary。
`python3 -m pytest -q tests/python/test_spec181_registry_policy.py tests/python/test_spec181_y_b_grant_seam.py tests/python/test_spec181_provider_grant.py tests/python/test_spec181_provider_lifecycle.py tests/python/test_spec181_plaintext_leases.py tests/python/test_ndnsf_di_provider_v3_boundary.py --tb=short`
→ **100 passed**，日志 `spec181-t001-registry-20260905-r1/final-identity.log`。
`python3 -m pytest -q tests/python/test_spec180_grant_provider.py --tb=short`
→ **7 passed**，日志 `spec181-t001-registry-20260905-r1/inherited-grant.log`。
原 Y-B seam 测试改为隔离 fixture，避免模块 import 时写全局环境或运行断言。

## Remaining Acceptance

这些结果使用临时测试密钥与受控 publication/fetch；没有真实 NFD
请求、MiniNDN 资格、SIF 或 Tiger 结论。T001 仍需真实发布/获取、
资源上界、完整封印绑定及取消/过期消费验收；T007 仍为 BLOCK。
