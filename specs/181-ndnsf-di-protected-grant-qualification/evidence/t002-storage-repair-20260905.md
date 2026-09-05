# T002 Native Storage Contract Repair

**Date**: 2026-09-05 | **Source baseline**: `778d69d9`
**Layer**: implemented + executed（定向 unit / 跨语言存储）
**Status**: PASS（本修复单元）；T002 OPEN / T007 BLOCK

## Change

新增 native `AssembledCiphertextV1` 存储实现：按既有双层 HKDF 派生
AES-256-GCM 密钥，对两种 entry kind 绑定独立上下文，认证实际磁盘
字节、全部 manifest 字段与大小上限。清理责任登记私有目录的已打开
描述符，取消时覆盖所拥有文件并删除目录条目；不跟随 symlink，路径
被替换时不删除替代目录。敏感临时 vector 由作用域 guard 清理。

双向比对发现 Python parser 未检查 manifest 的 `aead`、`kdf`、多余
字段及字段类型，现要求 canonical manifest 的完整字节一致。native
和 Python 采用相同拒绝边界，不再静默忽略声明的算法。

## Validation

原始数据保留在忽略的工作区临时根目录
`spec181-t002-production-20260905-r2/`。

- `storage-build.log` / `storage-test.log`：系统 C++ 编译真实 runtime、
  verifier 和 store；20 cases PASS。包含旧 18 cases 与两项存储/
  目录所有权回归；没有网络或 ORT 执行。
- `storage-final.log`：`python3 -m pytest -q
  tests/python/test_spec181_native_storage_parity.py
  tests/python/test_spec181_provider_grant.py
  tests/python/test_spec181_provider_lifecycle.py`；55 cases PASS。
  跨语言 adapter 直接调用生产 C++ store；两种 entry kind 实际落盘后
  双向读取，验证摘要/资源边界与 manifest 变异。装配 JSON 的摘要
  一致性只覆盖 storage context，不能作为 FR-012 ONNX 装配 parity。

## Remaining Boundary

本单元只提交存储与定向测试；已有 dirty-tree native factory、装配器、
executable 接线继续验证。真实 Provider 网络获取、ORT 模型生命周期、
取消与全部资源上界仍需其生产验收，不据此勾选 T002。
