# Development Disk Cleanup

**Status**: `CLEANUP_PASS` / no qualification change
**Spec**: `184-native-di-closure`

本轮仅清理可重建的开发中间文件和没有活动进程使用的用户缓存，不修改产品源码、
Spec184 合同、测试判据或外部实验状态。清理前根分区可用空间约 35 GiB（80%）；
完成后为 41 GiB（77%）。

## Removed

- `.codex-tmp/` 与 `pythonWrapper/build/` 中 3304 个普通编译对象/依赖文件
  (`*.o`, `*.o.d`, `*.d`)，合计 5,568,628,362 bytes（约 5.19 GiB）。
- `/home/tianxing/.cache/go-build` 与 `/home/tianxing/.cache/pip`，均无活动构建/下载进程。

删除前的路径与大小清单保存在
`.codex-tmp/cleanup-20260911-object-files.json`，删除结果保存在
`.codex-tmp/cleanup-20260911-object-files-result.json`。

## Retained boundary

保留当前 `build-spec184-b5-candidate` 及 `build-spec184-i02-asan-r2` 的可执行文件、
共享库和静态库，保留 Spec184 原始日志、候选记录、模型、SIF、密钥、源码和未提交改动。
VS Code C++ language service 仍在使用 `.cache/vscode-cpptools`，因此该 805 MiB 缓存未删；
HuggingFace 模型缓存也未删。一个已结束且会自我匹配的旧等待脚本已终止；用户正在进行的
`git add -p tests/integration-tests/ndnsf-di-core-flow.t.cpp` 未触碰。

对象文件可由对应构建重新生成；本清理不改变任何 C++ 测试结果、candidate digest 或
T007/T008 状态。后续若需增量编译，首次构建会重新产生这些中间文件。

## Post-cleanup smoke

使用保留的 current candidate 二进制、`NDNSF_SPEC182_BIN_DIR=build-spec184-b5-candidate`
和 candidate-first `LD_LIBRARY_PATH` 运行 C++ `Spec182PlanSealer` selector：12 cases，
exit `0`，`*** No errors detected`，elapsed `0:00.16`，max RSS `42036 KB`。
原始日志为 `.codex-tmp/cleanup-20260911-smoke.log`，SHA-256
`454360dcd46af3c15a65b399aab7646126b2913b21789f931c31961b763a0b71`。
