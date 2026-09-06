# Tiger Directory Migration

**Date**: 2026-09-06 | **Scope**: directory layout; no native implementation
**Status**: COMPLETE for relocation; tool-suite failure CLOSED by Review Sync R1 below

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
继续合并审查，将该工作树后续的旧路径修复映射至manifest目标；下方记录本次已同步范围。

## Review Sync R1

2026-09-06继续核对合并审查工作树：64个迁移文件、共享lib/bin及四个相关根目录发布/校验脚本均与本机一致，不重复覆盖。
两份测试存在已审修正，已逐字同步到本机：

- `tests/python/test_spec180_tiger_supervision.py`：无证据producer的fixture先被collector拒绝，因此预期原生异常类名CollectionError；仍严格断言返回失败、terminal FAILED、各readiness屏障与清理。没有修改生产异常处理或放宽通过条件。同步文件SHA-256为`10d4406986c9bf0e5ff9ed7cb73e46c90d280ea474a2f1d0266c550374492793`。
- `tests/python/test_spec175_tiger_profile.py`：临时添加共享库路径后在finally恢复sys.path，避免污染后续测试。同步文件SHA-256为`12a1a2f2c549819372f7b31c112b7bd0fc8049c09ff303f0faccd5f7a80b8cd0`。

静态审查对照execute→collect_terminal_result→validate_terminal顺序和collector负例的精确原因码断言后执行：

`python3 -m pytest -q tests/python/test_prepare_local_sif_source.py tests/python/test_spec180_release_workflow.py tests/python/test_spec180_tiger_contract.py tests/python/test_spec180_tiger_supervision.py tests/python/test_spec175_tiger_profile.py tests/python/test_spec180_terminal_collector.py`

结果 **58 passed in 8.11 s，exit0**。原始日志为`.local-tmp/tiger-review-sync-20260906-r1/unit.log`。
覆盖前轮51项和额外7项collector正负例；原失败记录保留，当前原因码断言已关闭。
本次提交包含这两份此前未跟踪的测试及同步记录；未提交Tiger作业和其他合并修复仍由原owner交付。
该结果仅是工具单测，不能升级为SIF/MiniNDN/Tiger或整个合并验收PASS；独立审查工作树未改动。

## Follow-up R2

2026-09-06复核：manifest的64个canonical文件均满足原SHA-256、文件权限与legacy路径解析一致；逐字对比审查工作树原路径无差异。共享lib/bin（排除生成的Python缓存）及R1列出的四个根目录工具共29文件亦一致。Tiger目录Markdown本地链接、共享lib/bin链接及本地历史SIF链接均可解析；两份R1同步测试与HEAD及审查工作树一致。没有新的脚本修复需要搬运，不重复运行未变单元的测试。

审查工作树`/home/tianxing/NDN/ndnsf-integration-182`仍有MERGE_HEAD，HEAD为`d4a5e39ce5b4a023f6e55d2440c60aa998983f8f`，包含未提交修复；本轮未修改该工作树。其忽略的临时目录内`merge-20260906/integration-static-r1/output.log`完整module结果为 **152/154 PASS、2 failed**，本次读取SHA-256为`72f8941b6840088e3d3b39236e347b7b92d488701bb460ce5f93e202e987cd0f`。这是对已有日志的读取，不是本轮新执行结果，也不能用HEAD单独绑定尚未提交的被测源码。

两个首边界为`Spec175NativeTinyOnnxExpiredDeadlineCleansProviderState`的providerFailures为0、预期2，以及`ProductionNativeHandlersRunD2h212ToCompleteOracleResponse`缺少角色和最终oracle响应；审查侧failure-log已登记。该工作树的`d2h-fence-trace-r1/output.log`也保留了后者定向失败。修复和回归仍归原审查owner；本轮不复制未完成的Core修复，不将这些失败归因于目录移动。

目录迁移已完成，R1的58/58工具单测证据继续保留；本轮仅追加上述路径/内容复核。原有9个已跟踪修改、15个未跟踪Tiger作业继续由原功能单元交付。下一步在合并审查形成最终基线后，按manifest映射剩余修复并核对共享依赖；届时再确定可供另一台机器使用的完整源码交付，当前checkpoint仍不代表完整发布。Spec182实现、SIF构建与MiniNDN/Tiger验收状态不变。

文档校验器PASS（110个本地链接），diff空白检查PASS。首次文档提交被仓库引用过滤钩子拒绝：临时目录全名属于禁止提交的工具标识；改为目录内相对标识，保留工作树位置和日志哈希后重试，不禁用钩子。该提交失败不是运行验证失败。
