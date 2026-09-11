# R11-B9-G10 Current C++ Spec182 Regression

在 `729555fa` client-close operation registry 修复和 `5528fa4e` manifest provenance
之后，使用同一 fresh `unit-tests` binary 重跑全部 `*Spec182*/*` C++ selector。
该批没有改变产品源码或 wire contract，只验证 registry 改动没有回归其它 native
组件和 lifecycle suites。

```text
timeout 240s .codex-tmp/spec182-t016-unit-20260911/build/unit-tests \
  --run_test='*Spec182*/*' --report_level=detailed --log_level=message
```

结果为 `260/260` cases、`7107/7107` assertions、process `rc=0`。原始详细输出与
rc 保存在 `.codex-tmp/spec182-g49-close-registry-20260911/spec182-full-detailed.log`
和 `spec182-full-detailed.rc`；静态门与 `git diff --check` 已在代码 checkpoint
前通过。

这只证明当前 C++ Spec182 regression lane；不等价于 maintained caller migration、
no-Python zero-use、完整 I01--I08/PO matrix、exact-SIF/多机或 T017 handoff，父任务
继续保持 `PARTIAL`。
