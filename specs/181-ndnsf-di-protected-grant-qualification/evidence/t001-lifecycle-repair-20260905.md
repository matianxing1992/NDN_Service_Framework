# T001 Plaintext Lifecycle Repair

**Date**: 2026-09-05 | **Source baseline**: `643e9c67`
**Layer**: implemented + executed（定向 unit）；完整 T001 尚未闭合。
**Status**: PASS（仅公共租约生命周期子单元）

## First Boundary

`SPEC181-T001-LIFECYCLE-R1`：新增 5 个聚焦测试，覆盖内存内容密钥租约、
重复 lease ID、单项清理失败后继续清理、私有文件权限与 symlink 拒绝。
原实现缺这些生命周期保证。命令：
`python3 -m pytest -q tests/python/test_spec181_plaintext_leases.py --tb=short`
（PYTHONPATH 为仓库的 DI、Core wrapper 与 Repo wrapper）。
原始结果保存在忽略的工作区临时根目录
`spec181-t001-lifecycle-20260905-r1/red.log`；这是 unit 失败，未启动网络。
修复后必须保持旧文件租约 API 兼容，并证明全部租约清理/失败保留语义。

## Repair and Closing Evidence

初始 RED：**5 failed**。新增内存专用 `register_secret`，不产生密钥
磁盘副本；重复 lease ID 在写文件前拒绝。文件以 0600 打开并拒绝
symlink，持有原文件描述符以防路径替换重定向零化；失败项保留待重试，
其余租约仍全部清理。context manager 覆盖异常退出。

追加路径替换与异常关闭用例后的命令：
`python3 -m pytest -q tests/python/test_spec181_plaintext_leases.py tests/python/test_spec181_provider_grant.py tests/python/test_spec170_artifact_security.py -k 'plaintext or lease or ProtectedAssemblyQualification' --tb=short`
→ **19 passed / 23 deselected**；日志
`spec181-t001-lifecycle-20260905-r1/green-path-substitution.log`。
中间同选择器结果为 17 passed（尚未加入两个增强用例），保留 green.log。

该子单元尚未把新的内存租约接入 Provider，不能关闭 T001；下一步
在生产入口建立统一 finally，并将 grant 验证移到装配之前。
