# Candidate and Validation Contract

## Candidate Identity

candidate family固定source（包括需要的overlay）、native executables/shared libraries及transitive runtime、Python wrappers、harness/oracles、effective config、Spec202 acceptance合同与外部小fixture。Spec/plan/tasks输入改动也必须登记失效范围。不能将private key/model/SIF/large logs放入Git。

不同server/topology/import profiles不能互换一个lock。允许同一冻结common-source下的显式profile composition：Persistent五host、object五host、six-host三Provider、seven-host selected三+unselected一、包含production importer profile。每个profile锁列清相同common-source hash与变化的declared flags/targets；两轮完整matrix都用同一组profile身份。

source closure执行门必须拒绝缺文件、source/artifact mismatch、dependency加载偏离、profile错配、Python模块缺失及不合适sudo env，mutation rejection发生于创建network/运行目录等昂贵副作用之前。不得只比源码git SHA或相信LD_LIBRARY_PATH变量；实际loaded libs路径与SHA进入manifest。

显式SDK source/build pair必须在每个实际consumer的编译include顺序中优先于共同global prefix，以实际compiler dependency证明使用的headers，不能只查Waf单个变量。OpenABE解析/deep-clone产生的policy独占其子树；root销毁必须释放全部descendants。SDK lifetime修复不改变publicABI、有效policy/crypto语义或通用ZObject计数约定；保留原SDK工作树，在隔离源中修复、审查及重建，并重做受影响动态门。

`spec193_preflight.design_acceptance_digest(path)`只对202 tasks/matrix生成显式acceptance projection，其余文件仍用完整SHA。tasks只排除标记包围的progress/checkpoint区并归一化checkbox，保留全部任务正文、依赖和design binding；matrix只排除标记包围的qualification status及明确表头所定义的round/native status cells，保留ID、scenario、oracle、owner及其余文字。缺标记、错表头/列数、缺少或重复任务/case立即拒绝。完整文档SHA保留在lock的`progress_provenance`，只该字段和Git revision不参与identity/replay equality。status-only edit必须稳定，requirement/dependency/design-binding/schema edit必须失效；不得泛化删除Markdown或豁免整份任务文档。

## Change Invalidation Matrix

| Change plane | Earliest required gate | Invalidated evidence |
| --- | --- | --- |
| requirement/contract/oracle interpretation | scoped code-aware convergence +traceability | 对应旧验收结论，不改旧原始run |
| native API/store/worker/codec | scoped static review →rebuild affected consumers →native unit/integration/dynamic | affected artifact/source family、Python native consumers及对应matrix |
| Python/DI facade | native binding/parity static +conformance | 对应P/跨语言/DI证据；ABI变化重建受影响consumer |
| harness/packet oracle | scoped static/source registration +closure | 对应case输出；offline reanalysis单列，不冒称重新执行runtime |
| runtime dependency/profile/topology/permission | closure +affected integration | 相关profile锁及network qualification |
| 拼写/不影响验收的说明 | 文档diff/links | 不无理由重跑昂贵matrix |

## Gate Order and Stop Conditions

source/intent audit →cohesive focused native red/repair →五lane fixed-snapshot static review →affected builds≤j4 →unit →生产integration/Python conformance →risk-selected dynamic →fresh design-code convergence PASS →完整MiniNDN two-round qualification。

未解决semantic/security/owner/wiring/evidence HIGH，产品convergence保持BLOCK；允许focused red/repair，不启动broad qualification。设计就绪与产品合格是不同结论。所有新任务含Design binding；不明确的入口/字段/迁移影响先补契约。

## Dynamic Gate Card

每logical batch一张：冻结source/toolchain/output、名义/边界/非法/lifecycle顺序矩阵、C++ selector、timeout/repetitions、oracle、首失败。

- destination/worker native修复：默认`asan-ubsan`，观察路径/FD/回调寿命、no UAF/leak/UB；若变更新并发ownership，则增加独立`tsan` gate。
- parser behavior出现实质变化：`parser-fuzz`有界输入语料并运行生产C++ codec；只做字符串文档纠错不虚构动态需求。
- docs-only/离线Python signed-packet oracle可理由化`none`；真实业务语义仍由C++/签名与storage oracle验证。
- 外部sanitizer ABI报告保留未抑制原日志并分类PARTIAL/DYNAMIC_FAIL，不能detect_leaks0或ASAN抑制升级PASS。

## Network Execution Contract

每次fresh output路径、untraced env（env-uNDN_LOG，trace vars unset）、sudo-n-E后设置实际PYTHONPATH/LD_LIBRARY_PATH，pinned SVS build在repository build之前；普通build/test unprivileged。首次fullrun写lock，后续相同profile引用该lock。five hosts为Controller/app/A/B/consumer；三Provider增加独立C，three selected+sentinel增加独立D。finite peers exit0；long-lived服务bounded SIGTERM可有-15，按manifest声明的预期停止分类，不能要求全部后台服务都退出0或把异常finite child -15当PASS。

确认每节点非空pcap、签名/response/receipt与storage实际owner、consumer digest、loaded libs SHA、cleanup结果；不复用output，不混Source，不用历史fixture代替当前候选。跨MiniNDN seed/read用保留Repo磁盘根、新进程新consumer，不重新STORE或借原source文件。

## Completion Contract

validation-matrix全部59 network case及其网络子场景都有当前共同source+profile的round1/round2证据。R01–R07属于最终candidate的native API/worker/import focused regression及适用dynamic gate，不要求将syscall故障注入移植到MiniNDN。全部affected suite/动态门与closing review通过。所有发现有owner/closing regression；failure-log包含首次失败和changed gate；可clean-checkout或明确sealed bundle复现。单轮A06、componentPASS、静态No findings、帮助/解析拒绝/链接smoke不能关闭qualification。
