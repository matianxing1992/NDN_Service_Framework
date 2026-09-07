# Spark Execution Preparation

**Scope**: documentation and skill adaptation only
**Status**: DOCUMENT_CHECKS_PASS / product NOT_STARTED / Spark trial NOT_RUN
**Source baseline**: `930fcf79` / Experimental，随后并行设计提交以实际最终checkpoint父提交核对。

## Change Boundary

将17个父任务展开为有限范围执行卡；每卡包含Parent/Depends/Read/Write/Steps/Verify/Reviewer，
阅读与写入路径分离、planned测试显式标注，T001设计者负责关闭缺口并release。
技能公共规则保存在仓库`skills/speckit-code-design/references/bounded-executor.md`；
本机`.agents/skills/speckit-tasks/SKILL.md`和`speckit-implement/SKILL.md`仅增加profile路由，
个人安装的code-design入口指向仓库权威。该本地目录受既有Git禁止路径规则约束，不强行纳入提交；
其他机器可直接按仓库技能和Spec入口执行同一profile，不依赖这两个本机补丁。

精确核对最新Host设计后补入Core scoped registration、共享ExecutionLeaseService、公共host三个行为卡。
T006原生worker真实进程负例转入T016执行，编写责任保留T006；work-units/proof/traceability同步。
现有FR/SC/CD/PO与17项checkbox未改变。没有产品源改动、构建、unit/integration/MiniNDN、SIF/Tiger运行。

## Retrieval and Existing Work

Context Mode stats仅作异常筛查。project health PASS；active health因tasks源hash已变化返回exit4，
当前状态使用实时tasks/contracts/git fallback，不从旧索引推断完成。project query第一次缺require被guard拒绝，
补明确identifier后guard和relevance查询成功；没有绕过guard或purge。最终索引检查另记下文。
CodeGraph已核对NativeEpochCoordinator/sampleToken和NativeStandaloneTokenizer现有调用路径；
最新typed-complex reference失败的boundary.json已读取，本轮不重试该算法/依赖探针。

预存/并行改动包括failure-log、ONNX normalization及Provider生命周期设计；本轮只提交执行包所有路径，
既有原始日志、临时构建目录和未跟踪工件不纳入Git。tasks使用新增独立段落保留其他设计记录。

## Validation

- Spec Kit prerequisites：exit0，活动路径182，contracts/tasks存在。
- `python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py specs/182-native-di-python-bindings --strict`：exit0/PASS，19 FR、11 SC、5 stories、17 tasks、0完成。
- `python3 specs/182-native-di-python-bindings/checklists/validate_design.py`：exit0/PASS，36张卡覆盖17父任务、依赖无环、211本地链接；19 FR/11 SC/14 CD/16 PO与12类137字段保持。
- 新validator三项内存反例：未知依赖T099-Z、T005-B自环、移除T017卡均被exit1及对应错误检出；未改变磁盘Spec或产品源码。
- source/read路径与符号定向核对：修正ONNX graph和Qwen generation的精确Python路径；planned头文件由前置卡或本卡明确创建。
- FR/SC/CD/PO标识集合相对HEAD保持；`git diff --check`：exit0。
- 官方skill quick_validate对仓库code-design：PASS。对两个既有Spec Kit入口因原有顶层`compatibility`字段返回不支持；未改其现有格式迁就通用校验器。改用YAML解析/name/description及profile路由检查，两入口与个人code-design入口均PASS。这是校验器schema不匹配，不是执行卡或产品运行失败。

本记录不把结构、链接或技能语法检查称为Spark能力实测或产品资格。上述211为本次快照，后续并行设计加链接不使该历史计数失真。

最终刷新已完成，project health PASS；active strict仍因另一工作单元继续修改tasks返回stale hash/exit4，
因此继续使用实际文件fallback，不宣称活动索引新鲜。无需为文档分派包修改或停止其他设计工作。
首个failure-log选择性stage patch未含尾部上下文，`git apply --cached --check`拒绝且未改index；
改用HEAD与本单元精确新增段落生成标准上下文patch，failure-log只stage本条。
并行提交`1c3d7602`已包含本单元tasks段落；本轮检测到该事实后不重复stage、不改写它的API inventory或历史。
本轮其余执行包以该提交为父基线保存；ONNX normalization草稿和typed-complex失败条目保留原未提交状态。

## Next Action

T001-C按最新设计证据完成release；不得为了试用Spark跳过T001或已冻结的父任务依赖。
之后执行T002-A及后续就绪卡，记录真实首次验收、审查修正和越界修改；T015整体审查及T016真实资格保持。
