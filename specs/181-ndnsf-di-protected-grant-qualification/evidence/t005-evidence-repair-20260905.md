# T005 Evidence Preservation Repair

**Layer**: implemented / focused regression
**Status**: PASS (focused repair); T005 qualification NOT PROVEN

## First Boundary

旧 `scripts/run_spec181_y_n_matrix_retry.py` 删除旧 attempt 目录与
`/run/nfd`，无条件重试每个子用例，并拼接各自首个 PASS。入口绕过
维护 runner 的 Y-N-E 三变异聚合；其自有结果不能形成同源资格。
维护矩阵收集器也会在控制/负例失败后继续后续子用例。

`spec181-t005-evidence-repair-20260905-r1/red.log` 位于 ignored
workspace temporary directory，保留新增定向回归的 5 个失败。检查只使用
临时文件、替身启动入口与隔离 Python CLI，不启动 MiniNDN/NFD。
T007 BLOCK，未执行正式矩阵。

## Repair Decision

没有已验证的启动前可重试分类器，取消旧自动重试入口，保留明确
exit 2 的兼容提示；维护 runner 是唯一矩阵入口。每次失败先保留
证据、诊断并更新 failure index，再由操作者使用新 run-id 执行。
维护矩阵遇到首个失败停止，保留该子用例原始结果，不生成聚合 PASS。
源/构建/配置身份仍由维护 runner 的输入及构建门和 T008 同源验收负责；
停用入口不执行运行，也不伪造新的身份或资格报告。

## Focused Verification

`spec181-t005-evidence-repair-20260905-r2/focused.log` 记录 118 PASS：

```bash
env PYTHONPATH=NDNSF-DistributedInference:pythonWrapper:NDNSF-DistributedRepo/pythonWrapper \
  python3 -m pytest tests/python/test_spec181_y_n_matrix.py \
  tests/python/test_spec180_yolo_minindn.py \
  tests/python/test_spec181_production_grant_mutations.py -q
```

验证旧入口不触碰既有失败文件、不启动 runtime，隔离 Python CLI
无需运行时依赖即可返回明确停用原因；控制、普通负例、grant 变异
各自失败后均不运行后续子用例，也不覆盖原始失败或生成聚合 PASS。
这些是文件/调度器定向回归，不是七子用例网络资格。源码基于
`43061600` 加本轮改动；T005 未勾选，T007 仍 BLOCK。
