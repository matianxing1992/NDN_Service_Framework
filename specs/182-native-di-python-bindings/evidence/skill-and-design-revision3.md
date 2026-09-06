# Skill and Design Revision 3 Evidence

**Date**: 2026-09-06
**Status**: DOCUMENT_REVIEW / runtime NOT_RUN
**Scope**: 修改设计规则与Spec182，读取合并源码；没有修改运行实现或重试合并失败。

## Source Authority

设计文档在主工作区维护；源码审查针对 `/home/tianxing/NDN/ndnsf-integration-182`。
该工作区正在合并修复，HEAD为`d4a5e39ce5b4a023f6e55d2440c60aa998983f8f`，
MERGE_HEAD为`4391af81cd24ff5510aa52b48ab9cec0fdec1ebb`。没有文本未解决冲突不等于合并已提交或通过测试。
[27-file snapshot](merged-source-baseline-r3.json)记录实际文件hash和状态；后续修复若改变文件，T001必须刷新身份，不能复用本次静态观察作资格证据。

持久输入是合并工作区的 `specs/182-native-di-python-bindings/evidence/integration-20260906.md` 及其所链raw记录。
已读取 unit R1/integration R1 的失败边界：747/751与60/92，分别exit201和exit139；segfault与fixture/ABI接线修复仍在进行。NAC-ABE46/46和Context45PASS是独立范围。本轮没有运行这些测试；引用是既有记录，后续结果可能推进。

## Changes and Rationale

1. 更新本地speckit-code-design主技能、design-template、review-gate、work-unit-contract，新增symbol-contract参考规范；specify/plan/tasks入口强制接入同一规范。
2. 规范要求What/Why/How/Usage，覆盖类型/重载/参数/字段/关键状态及注释；LOCAL_DETAIL只用于不影响契约的普通内部细节，不能用来跳过密钥/锁/期限/配置/状态字段。文档中没有实现的示例必须标NOT_COMPILED。
3. Spec182 revision3新增FR-017/SC-009、21类/模块和48方法条目、12来源类型137字段对照，并向17个任务加入SymbolContracts/Documentation/Usage。
4. 修正旧181完整资格前置、合并Core安全/撤销继承，以及CandidateBudget的实际三个字段。旧证据保持历史身份。
5. 来源字段对照不等于完整原生ABI：嵌套schema、tokenizer/ONNX库选择、注册/取消细节和完整caller/journal映射仍OPEN。明确阻塞对应实施，不能以结构检查PASS称READY。

## Local Skill Delivery

技能存储属于本机配置，仓库提交钩子排除该类本地配置路径；本次技能修改保留在本机，不混入源代码checkpoint。源库中的Spec附件和验证器可提交、可审查、可随开发交付复制。技能正文与入口引用的静态核对结果记录于本次验证记录。没有将本机技能修改声称为可从仓库commit恢复。

## Validation

文档验证器首次执行因新增正则表达式双重转义产生假缺失（类/方法/字段/任务匹配全部未命中）；首边界是检查器文本匹配错误，不是设计覆盖或运行时失败。修正转义后重跑，旧结果不计PASS。

执行记录见 [document validation](revision3-validation.json)。检查覆盖FR/SC/CD/PO、17任务有向依赖、链接、符号/来源字段唯一性、任务文档/用法条目和差异格式；它只证明文档结构与覆盖清单一致。运行时build/unit/integration/MiniNDN均NOT_RUN。

Context Mode项目层健康，活动层曾因旧managed plan指181失败；本轮修正本机managed引用后刷新authority并单独核对。源码工作区无索引，按规则使用精确文件和AST。未运行外部研究或推断未安装原生库可用。

## Checkpoint and Next Action

末次源码漂移核对：ServiceController.cpp及合并integration记录在本轮期间继续更新。新增readiness设计使用有界challenge再获取精确PUBPARAMS，不能恢复旧任意名称后缀行为；该修复仍由合并工作单元验证。原27文件快照保留观察时身份，不能冒充最终合并版本。12个来源类的137字段经再次AST对照无增删或顺序差异。

实现任务0/17。合并修复与最终checkpoint由当前修复工作单元完成；随后T001刷新source identity，关闭O-001--005并细分可验收实施单元。当前不进入T002，不将本次技能/文档检查写作任何运行资格PASS。
