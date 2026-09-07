# Task Progress Registry Migration

## Scope and Baseline

2026-09-07：将 17 个父任务下的 36 张执行卡全部登记到 tasks.md 顶部，
通用执行定义改用 contracts/execution-units.md；旧 Spark 路径仅保留历史兼容入口。
当前状态只维护一份，设计契约与历史证据不再兼任当前状态表。

读取时 HEAD 为 `aba90194`。此前 `48b1c7b9`、`3afa7492`、`547dd585`、
`e17e66f5` 提供 planning/tokenizer/lifecycle、assembly/grants/preparation/bindings、
isolation runner 和 closure guards 的源码切片。PARTIAL 表示相关切片已有持久记录，
不表示本卡全部行为已实现或验收通过。NOT_STARTED 表示未找到本卡独立执行记录，
不证明所有相关代码都不存在；接手者应核对复用，避免重新实现已有部分。

T001-A/B 按设计 checkpoint 登记 PARTIAL；O-004 公开 API 清单与整体 release
未关闭，T001-C 保持 BLOCKED。T002-A 已有安装 consumer 工作，但构建仍受
NAC-ABE Consumer/CacheProducer API 闭包阻塞：
`.codex-tmp/spec182-native-build-20260907-r1/result.txt` 为 exit=1 / boundary=build，
列出缺失 getPublicParamsDataName/getPublicParamsDigest/clearCache/
refreshPublicParameters/refreshDecryptionKey。原失败索引及证据保持原位。

CodeGraph 核对 NativeInferenceClient 的当前 request 路径仍以
`NATIVE_REQUEST_PIPELINE_NOT_READY` 终止，因此 T010-B 只能 PARTIAL。
既有 checkpoint 的 Python focused 17/17 与 C++ 语法检查只沿用其原边界；
本单元未重跑产品检查，全部父任务仍未勾选。进度迁移不能替代逐卡接线审查。

## Validation

本单元仅进行文档/技能规则与进度校验器修改，以下检查实际 PASS：

- `python3 specs/182-native-di-python-bindings/checklists/validate_design.py`：
  36 卡/36 行，22 PARTIAL、2 BLOCKED、12 NOT_STARTED，父任务 0/17；最终 298 本地链接有效。
- `.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks`：活动 Spec182 入口正确。
- 内存注入三个反例：删除进度行、重复 ID、用迁移 baseline 冒充 DONE；均 exit=1 且报告预期错误。
- `python3 /home/tianxing/.codex/skills/.system/skill-creator/scripts/quick_validate.py skills/speckit-code-design`：PASS。
- `git diff --check`：PASS。

共享技能规则可随仓库交付。本机 `.agents/skills/speckit-tasks/SKILL.md` 与
`.agents/skills/speckit-implement/SKILL.md` 同步通用入口，按仓库规则不纳入 Git。
产品 native 构建、unit、integration、MiniNDN、SIF/Tiger：NOT_RUN。

Context Mode active health 报告索引过期，本次状态取实际 tasks、Git checkpoint 和源码，
不用检索摘要宣称任务完成。随后刷新 authority 索引，project/active health 均 exit=0 / ok=true。
两个本机 Spec Kit router 的 YAML 与通用进度入口检查 PASS。
预存依赖/生成设计与 failure-log 改动属于其他工作单元。

## Next

关闭 T001 未决公开 API/设计与 selector release；修复既有构建依赖边界。
后续每个单元结束更新进度行并替换为其精确证据链接，沿 plan Gate Order 推进。
