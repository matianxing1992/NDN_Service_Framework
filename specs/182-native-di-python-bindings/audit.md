# Spec182 Design Audit

**Revision**: 7 | **Mode**: source alignment / pre-implementation
**Verdict**: DRAFT / BLOCK for implementation
**Source**: `81e251a4ef1d8e6a394dc5f0c38bc44e44bfc973` / Experimental
**Evidence**: [current source and dependency baseline](contracts/integrated-baseline.md)

## Current Findings

revision 7审计已按最新源码修订文档；用户随后授权在Experimental完成182，当前T001设计收口中。已停止管理已接收交付的实验机器。
源码身份与历史状态的文档漂移已修订；未实现功能继续planned，未闭合设计继续BLOCK。
T001有界依赖探针见[native dependency design](contracts/native-dependency-design.md)，不计产品实现或T015/T016资格；后续unit/integration/MiniNDN按任务门执行，SIF/Tiger由外部负责。

| Finding / severity | Source evidence / controlling requirement | Correction / owner / closing proof |
| --- | --- | --- |
| A7-01 / HIGH / RESOLVED | spec Relationship/Assumptions与symbol readiness仍写未提交合并、integration失败；实际HEAD包含整合历史，生产路径与merge及交付源无diff；FR-015 | 更新当前baseline、分离历史checkpoint，O-001仅按源码范围CLOSED；T001仍未完成 |
| A7-02 / HIGH / RESOLVED | baseline原标VALIDATED却未包含SVS `9f2d8a47` / NDNSD `375a35c5`；当前lock和停止记录明确ABI消费者未全验证；FR-012/014，INV-008 | 固定四库pin与ABI失效范围；旧759/154结果保持历史，当前组合UNQUALIFIED；CD-009设计、T015审查、T016运行证明 |
| A7-03 / HIGH / OPEN | ServiceProvider.hpp公开addService/addCollaborationHandler，无逐服务注销；CD-014/M47需要共享宿主close语义；FR-008/010/017 | runtime-boundaries补确切接线及缺口；T001/O-004冻结registration/ACK/Selection fence、lease共享和重复注册，T009实施，PO-014检出误停共享服务与晚到工作 |
| A7-04 / MEDIUM / RESOLVED | requester仍在app_sdk/placement.py::_request_v3；NativeCanonicalOnnxAssembler.cpp::runPythonHelper与NativeStandaloneTokenizer.cpp::makeNativeStandaloneTokenizerDecoder仍启动Python；独立DI库/新facade不存在；FR-001/006/007/012 | 保持CD-001/005/006/009为planned，复用现有Provider/安全/epoch机制；T002/006/007/010及PO-001/005/006负责目标实现与证明 |
| A7-05 / MEDIUM / RESOLVED | 旧baseline链接、revision2现行声明、交付任务混入Current Checkpoint；当前root skills与Tiger交付工具已存在；FR-014/017 | 当前authority统一指integrated-baseline，旧checkpoint标历史，plan/T017复用共享技能和工具；旧Python交付模板不冒称182 no-Python成果 |

| Open item | Controlling gap | Owner |
| --- | --- | --- |
| O-001 / CLOSED | 当前源身份、合并差异及181承接已核对；不等于当前依赖运行PASS | T001部分完成 |
| O-002 | ONNX原生装配字节契约与依赖锁 | T001 |
| O-003 / CLOSED | 精确crate/toolchain锁、C ABI/释放/串行寿命/生产路径及84对照+14负例PASS；产品迁移/隔离尚未完成 | T001设计完成；T007/T016实施证明 |
| O-004 | 12类137字段与当前源一致；完整旧能力/调用方/selectors、嵌套schema及A7-03注册寿命仍缺 | T001 |
| O-005 | 无Python隔离方案可行性与边界 | T001 |

O-002--005的有界关闭条件见[code-design](contracts/code-design.md#open-questions)，依赖探针结果与完整算法/兼容设计关闭分别记账。
T002--T014的各项实现、测试工具编写、静态审查、局部单测完成后，T015补审整体接线，
T016收齐真实运行证据，T017交付。验收标准满足即结束；变化或具体缺陷才触发受影响回归。

## Source Checks and Limits

- `git diff --name-only c770f18b HEAD -- ndn-service-framework NDNSF-DistributedInference NDNSF-DistributedRepo NDNSF-UAV-APP examples pythonWrapper wscript`与同范围`447f7584..HEAD`均无差异；读取四仓库HEAD与交付lock，核对祖先关系。tracked源码无预存改动，本地未跟踪日志/构建目录不纳入审计或提交。
- CodeGraph先查生产符号，再精确读取ServiceUser.hpp、ServiceProvider.hpp、DI_NativeProviderExecutable.cpp、两helper、examples/wscript及Python requester。宽泛结果混入`.codex-tmp/compare-*`，拒绝其作为当前源码证据，不重建索引或扫描整个临时树。
- source-field-coverage.json的12类137字段按当前Python AST核对名称/类型/默认值；这只覆盖已有表，不证明全部嵌套schema或公开调用方穷尽。现有137字段表保留，不重复建立第二份DTO权威。
- 原生Provider接线必须保留execution lease服务、V3 offer、provisioning/readiness、permission、protected preparation、epoch与结果路径；纯C++host不是只包装最终runtime.handler。Core控制publication不是通用remote-abort。
- 任务仍为17个行为单元，未因审计机械拆分。FR-001--019、SC-001--011、CD-001--014、PO-001--016及既定负例保留；实施相关unit与T016完整集成/MiniNDN的分工不变。
- Context Mode project/active健康检查通过；宽泛`status` timeline查询被guard拒绝，改用精确file-backed active tasks的relevance检索并对照源码文档。持久文件为authority，不用旧session状态裁决。

文档检查使用`check-prerequisites.sh --json --require-tasks --include-tasks`、`audit_speckit_structure.py ... --strict`、`checklists/validate_design.py`及`git diff --check`；实际结果记tasks的当前checkpoint。它们不是产品测试，也不能关闭O-002--005。

## History

既有审计发现的历史理由与证据保留在Git及
[revision 2](evidence/audit-revision2.md)、
[revision 3](evidence/skill-and-design-revision3.md)、
[revision 4](evidence/static-review-gate-revision4.md)、
[revision 5](evidence/adversarial-review-revision5.md)。
旧revision中固定五项风险、独立S0/S1报告与逐任务integration要求由revision 6替代。
历史源码快照与运行结果不回填为当前实现或资格证明。

## Next Action

T001继续关闭O-002--005：固定ONNX/protobuf字节契约、tokenizer ABI、完整兼容/注册/状态设计与独立测试selector、no-Python隔离方案。无需重开合并或续跑181资格；各设计项满足其完整关闭条件后才关闭。
任务完成 **0/17**；T001依赖探针单独记录，T015产品收敛审查及产品构建/unit/integration/MiniNDN **NOT_RUN**。
