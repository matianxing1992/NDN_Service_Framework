# Sealed Grant Reference Repair

**Layer**: implemented + executed（unit）；无 network、MiniNDN 或 qualified 声明。
**Status**: PASS（仅本项修复）。
**Date**: 2026-09-05。
**Source identity**: `67194dc2` 加本记录同一 checkpoint 的 Provider 与回归修改。

## Finding

`provider.py::_qualify_protected_assembly` 校验签名及 request/core/model/epoch，
却未比较接收 grant 与 Selection 中的 `GrantBindingV1.grant_digest`。
同一权威另签的相同上下文 grant（含不同内容密钥）可被接受。精确名获取
本身不能证明 payload 与被封印引用相同。

## Repair and Validation

在解包、密文暂存与明文租约创建前比较两个摘要，不匹配以
`ProtectedGrantRejected` 拒绝；外层继续使用 `DI_PROTECTED_GRANT_REJECTED`。
正路径、错误收件人、过期、跨请求与发布 seam 回归保持通过。

统一环境：`PYTHONPATH=NDNSF-DistributedInference:pythonWrapper:NDNSF-DistributedRepo/pythonWrapper`
（实际执行使用仓库绝对路径）；`python3 -m pytest -q`：

- RED：`tests/python/test_spec181_provider_grant.py -k valid_grant_cannot_replace --tb=short`，
  **1 failed / 32 deselected**；失败为 `ProtectedGrantRejected not raised`。
- GREEN：`tests/python/test_spec181_provider_grant.py tests/python/test_spec181_y_b_grant_seam.py --tb=short`，
  **34 passed**。

原始日志：忽略的工作区临时根目录下
`spec181-audit-repair-20260905/grant-reference-{red,green}.log`。
测试使用真实密码学与 Provider 方法、注入获取回调，证据层严格为 unit。
该修复未闭合 T001 的真实发布/获取、前置授权及完整明文清理验收。
