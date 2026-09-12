# C-04 Validation, Coverage and Migration

## Workflow Authority

使用仓库版本化 [batch-quality-gates](../../../skills/speckit-code-design/references/batch-quality-gates.md)
和 [review-agent profile](../../../skills/speckit-code-design/references/review-agent.md)。
实际官方技能为 `/home/tianxing/.codex/skills/review-agent/SKILL.md`；每任务与批末记录读取路径、
SHA-256、base/head/full diff scope、五lane实际查询、发现/修复/复审，不能只写 No findings。
本规划未运行实现静态门，下面是待执行的验收契约。

## Five-Lane Coverage Plan

每个批次的 evidence 文件保存此表的实际查询结果及 covered/N/A/gap；路径和代码必须引用当前版本。

| Lane | Required scope / query | Oracle |
| --- | --- | --- |
| production entry/callers | CodeGraph explore Runtime/PreparedModel/NativeInferenceClient；再 rg 核对 examples、Python binding 和维护APPClient | 新旧caller真正进入唯一native owner；配置生效、无fallback |
| implementation/wire | CodeGraph node 相应owner；rg request/attempt/grant/epoch/close；检查完整diff和surrounding code | C-01至C-06 identity、权限、终态、lease不变量 |
| test/harness/oracle | 读取tests/unit-tests或integration-tests的Spec185 suite和fixture析构；核对tests/wscript注册 | C++独立数值/状态/内容判据；不能重写生产hash充当唯一oracle |
| build/source closure | root wscript、examples/wscript、tests/wscript；symbol→definition TU→target map；构建后nm -C/readelf -d、binary SHA | 实际target输出包含生产实现，candidate receipt来自同一binary；无旧.so混用 |
| migration/evidence | rg全部维护NativeInferenceClient/APPClient调用方；caller matrix、184边界、Design/current-target | 每个mode有owner和真实route；历史资格不自动继承 |

B9是纯文档单元，动态profile为none；只验证文档与证据身份，不重跑已通过的原生链。

新target或跨库符号出现时，必须在构建前补definition map；发生undefined reference不能只重试链接。

## Dynamic Gate Cards

所有suite均待实现、注册后才运行；以下冻结行为等价类与预算。执行时在本批evidence填
source HEAD/diff digest、compiler/linker/dependency closure、实际C++ binary/selector、输出目录、
profile参数、oracle结果。未填不能 DYNAMIC_PASS。独立sanitizer构建，同ABI依赖；无抑制日志保留。
每case两次、单次60秒、每批30分钟上限；长模型process case单次180秒且总批60分钟，
超预算保留NOT_RUN，不自动扩张。测试时钟可以加速timeout，真实process至少一个真实deadline反例。

| Batch/profile | Dynamic Parameter Matrix | Business invariant |
| --- | --- | --- |
| B0/asan | normal+ASan各自同配置consumer；每公共安装头；ONNX enabled/disabled各自外部consumer构造析构；缺失include反例 | 安装闭包不依赖源码树与Python |
| B1/tsan | nominal open/close；zero cap非法；close twice；callback close；最后child先/后于Runtime释放 | 无self-join/data race/UAF；drain后owner=0 |
| B2/tsan | 1/8waiter；单/全部cancel；job deadline先/后于waiter；READY/PREPARING/ABSENT×4policy；预算刚好/少1字节；refresh成功/失败/逆序 | 单flight、隔离、旧lease可用、READY无半成品 |
| B2E/tsan | duplicate/replace/freeze；并发lookup；协作超时/cancel；旧插件；runner隔离 | 过期不发布Selection，不共享可变runner |
| B3/asan-ubsan | inline/repo；零/超限输入；wait<request deadline；cancel前/后终态；revoked hot cache；wrong role/digest | 独立request/grant；错误不执行；terminal单一 |
| B4/asan-ubsan | 1/2turn；同时第二turn；错model/tokenizer；commit前/后cancel；export fault；Provider replacement | journal/handle一致；旧attempt不推进新turn |
| B5/asan-ubsan | cold/hit；同/异role、ABI、epoch；无Selection/撤销；stop于fetch/assembly/run；两请求KV不同 | unauthorized fetch=0；无mutable state共享；lease归零 |
| B6,B8/none | old/new caller；wrapper默认/异常/迟observe；GIL释放期间close | native行为由B1–B5和T011process证明；若新增native生命周期则重开相关动态卡 |
| B7/asan-ubsan | unary/stream；2turn/recovery/replacement；revoke与cache hit；wrong content；deadline；正常/失败cleanup | C++独立结果正确、跨进程身份一致、私钥隔离、残留为零 |

动态工具报告内存/线程/UB，C++断言判断业务。ASan ABI噪音也须保留首报告；不能通过关闭LSan
或抑制后把失败提升为通过。parser恶意输入为语义反例；持续fuzz平台不是本Spec强制范围。

## Batch Result Record

一个批次一个evidence，包含：source/base/diff、Batch growth decision、Review trace及五lane矩阵、
stable exit、Closure decision（OPEN_FOR_NEXT_BATCH或CLOSED_FOR_VALIDATION及触发条件）、
build目标/实际路径/时间/身份、Dynamic gate card/matrix和每case结果。
Batch Retrospective 必须分别写 static、compile/link、runtime/test、unobserved，
失败首边界与重试前Changed gate。某lane未观察就是gap，不能把格式检查计为生产STATIC_PASS。

## Final Qualification and Source Scope

T011先跑安装后C++例子及最小进程unary/stream；T013在全部native实现/接线完成并fresh convergence后
完成C-06全部模式与反例，通过后才T012。SC-005包装行此时待验，不能反过来推进原生资格。C++进程fixture启动独立authority/requester/provider，native oracle验证
结果、receipt/plan身份及退出/cleanup；Python只允许编排外部设施和绑定自身断言。
native部署进程不得依赖Python planning；ELF依赖闭包与子进程清单共同检查。
仅tiny ONNX与已具备的Qwen/YOLO fixture可作为本地有界验收；大模型/实际硬件的未执行行明确标
外部或未观测，不据此声称Qwen3.6-27B生产资格。185任何新行为未验收不得交给184掩盖。

## Migration Registry Schema

T011在 `contracts/caller-matrix.md` 写实际盘点结果：path:symbol、language、mode、maintained owner、
old entry、new entry、source identity、C++证据、binding证据、status、retained reason、removal condition。
所有新增公共符号和安装头消费示例覆盖；混合历史Spec冻结fixture单列，不自动改旧证据。
T012精确定位实际binding TU后更新映射并迁移，不另建与原模块断开的绑定库。

## Additional Native Oracles

T003/T004覆盖prepareAsync迟completion、waiter取消和共享job；T006覆盖可靠completion及EventReader next/nextAsync单游标、超时、STREAM_GAP、EOF和回调析构。observe丢弃计数仅作诊断，不能证明token完整。
T009覆盖Provider-only配置及独立生命周期；T011/T013以安装prefix构建consumer，记录ELF及子进程no-Python闭包。Python future桥接native回调，不用Python线程补业务能力。

C-06原生drainAsync及CompletionSubscription归T002/T004/T006/T009和T013矩阵：退订在排队前/后、callback已运行、close/stop、清理成功/期限到及owner释放；禁止阻塞IO或用request完成代替cleanup。
