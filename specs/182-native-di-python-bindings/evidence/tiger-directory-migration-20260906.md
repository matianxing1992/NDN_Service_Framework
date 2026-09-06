# Tiger Directory Migration

**Date**: 2026-09-06 | **Scope**: directory layout; no native implementation
**Status**: COMPLETE for relocation; existing tool-suite failure remains OPEN

## Subject And Review

精确输入清单见 [migration manifest](../../../Experiments/TigerCluster/migration.json)。64个文件移到Tiger目录，逐文件内容SHA-256与执行权限均保持一致；原目录层级相同，四个legacy入口和共享lib/bin链接均指向本工作树。
静态审查覆盖脚本ROOT/HERE推导、共享库导入、profile的固定路径/哈希，以及源码sealer物化旧镜像路径的行为。18个Python语法、30个shell语法、3个JSON及相对文档链接检查通过。

## Unit R1

`python3 -m pytest -q tests/python/test_prepare_local_sif_source.py tests/python/test_spec180_release_workflow.py tests/python/test_spec180_tiger_contract.py tests/python/test_spec180_tiger_supervision.py tests/python/test_spec175_tiger_profile.py`

结果50 PASS / 1 FAIL，exit1；原始日志为 `.local-tmp/tiger-directory-migration-20260906-r1/unit.log`。
失败首边界是监督器零退出负例的原因码：测试期望TERMINAL_RESULT_MISSING，实际CollectionError。
源码先调用collector，再validate_terminal；缺少collector输入时已经失败，尚未到后者的缺失结果判断。
这是工具测试结果，不是Tiger或协议运行。下一步在无迁移的独立布局复制相同字节，运行同一具名用例判定基线原因，不改监督逻辑或放宽断言。

## Baseline Comparison And Result

将相同supervisor/collector/test字节复制到独立、真实的legacy目录布局，运行同一具名用例，
同样exit1并得到CollectionError != TERMINAL_RESULT_MISSING。原始日志为
`.local-tmp/tiger-directory-migration-baseline-20260906-r1/unit.log`。
该诊断用`-c /dev/null`禁用仓库配置，同时产生3条pytest cache目录权限warning；
失败本身仍是同一语义断言，未被启动/权限错误替代。既有失败保留给工具owner，不放宽测试断言。

新路径下build-local-sif.sh和submit_profile.py均正常打印既有usage并按原契约exit2；
replay-exact-sif.py的--help exit0，证明新路径入口/共享导入可加载，不代表实际构建或提交。
迁移的64文件、18 Python/30 shell语法、3 JSON、相对链接和执行权限检查PASS；
源码打包、profile与发布等相关工具单测共50 PASS，不能将含基线失败的51项套件写为全部PASS。

## Checkpoint Boundary

本地提交只记录49个此前已跟踪文件的纯路径迁移、兼容链接和本轮说明；
这些文件提交内容沿用原HEAD blob，迁移前已有的工作区修改仍保留在新路径且不纳入提交。
15个原本未跟踪的文件已移动，但继续保持未跟踪，等待所属合并/功能单元提交。
manifest中的哈希描述本轮实际工作区输入，不宣称该checkpoint为干净完整发布。
历史SIF仅增加本地链接，未复制、修改、重新封印或上传；合并审查工作树未改动。

## Next

Spec182原生实现仍0/17；SIF构建、MiniNDN及集群执行NOT_RUN。
继续合并审查，将该工作树后续的旧路径修复映射至manifest目标；工具owner修复已确认基线断言后运行相关回归。
