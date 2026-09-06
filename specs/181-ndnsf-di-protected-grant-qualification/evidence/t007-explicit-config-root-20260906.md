# Explicit Protected Configuration Root

**Status**: PASS (explicit config selection only; T007 BLOCK)
**Evidence layer**: executed (focused launch boundary)

## First Boundary and RED R1

`_run_live_case_once` 在 Y-B/Y-N-E 的受保护 epoch 中无条件覆盖
`NDNSF_SPEC180_CONFIG_ROOT`，因此显式配置无法到达后续 registry
loader；显式目录缺少 authority key 时还可能改用默认目录的 key。
修复限于维护 runner 的路径选择，不修改 grant/wire 或密钥算法。

R1 **3 failed / 1 passed / 7 deselected in 0.81 s**：默认路径控制
通过；显式绝对路径、相对路径和缺 key 拒绝均失败。fixture 在
native preflight 前停止，禁止启动网络，不消费示例密钥内容。
命令：`python3 -m pytest -q tests/python/test_spec181_recipient_algorithms.py -k runner_preserves`。
原始 `focused-red.log` 与 `source.patch` 保留于 ignored workspace
temporary directory 的 `spec181-explicit-config-root-20260906-r1/`。

## Repair Contract

显式配置优先；仅未配置时使用 operator HOME 的默认目录。在
MiniNDN 改写 child HOME/cwd 前将选中目录解析为绝对路径，沿用
既有缺 key 拒绝。显式目录不可用时不得回退默认目录。该单元不
代替密钥字节/身份核查、完整 local matrix 或 T007 的剩余收口。

## Focused GREEN R2

R2 **24 passed in 0.85 s**。命令：
`python3 -m pytest -q tests/python/test_spec181_recipient_algorithms.py tests/python/test_spec181_y_b_grant_seam.py`。
四种配置路径与既有 recipient 算法、grant seam 验证通过。默认
目录有 key 而显式目录无 key 时，仍在 native preflight/网络前
拒绝，不会回退；显式绝对/相对目录实际到达后续 child 环境。
`focused.log` 与最终 `source.patch` 保留在 R2 raw run 目录。

本单元修正配置选择，未运行正式 MiniNDN。T007/A05 继续核查
实际外部输入字节/import 身份与证据归集，再刷新本地 native receipt。
