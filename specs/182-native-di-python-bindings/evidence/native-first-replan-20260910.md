# Native-First Replan 2026-09-10

## Scope and Decision

本轮仅修订 Spec182 执行设计与 TG-02，不修改产品源码，不启动 native 编译、模型运行或 MiniNDN。
按用户决定采用 [N1–N5 契约](../contracts/native-first-execution.md)，在
[tasks.md](../tasks.md#native-first-dispatch-2026-09-10) 登记 R11-B1 至 R11-B9。
保留 T001–T017、历史结果及未完成状态；新批次均 NOT_STARTED。
下一批为 R11-B1：冻结 authority 传输/配置/生命周期契约，移除 requester 生产私钥边界，
完成 C++ 进程隔离证据后推进 R11-B2；不重启全 Spec，也不要求一次新的全仓库扫描。

## Source and Conflict Evidence

- 核对起点 `89932fb5b91f488eec6d4dc8756abba3fdaa40bd`。CodeGraph 的宽泛符号查询包含无关结果，随后按真实路径 `examples/DI_NativeRequester.cpp` 核对；132 行读取 `authority_private_key_file`，138 行配置 `authorityPrivateKey`，156 行构造 `NativeArtifactGrantIssuer`。这些是待改的生产边界，文档重排不代表已修复。
- CD-004 的进程内 authority 例外、FR-019/SC-010 和 plan/proof/work/execution cards 的“所有真实进程运行留 T016”与新顺序冲突，已以当前规则取代；历史 revision/运行记录不重写。
- 已阅读 R10-B83/B84 的边界和 B84 原始失败日志：曾有 ACK 但 `collaborationCalls=0`，后续具名 C++ 用例的修复不等于独立进程资格。保留其定向证据，不重复实验。

## Coverage and Static Review

本次为一个文档批次；静态检查对象为变更条文、跨文档依赖、旧状态保留及 PDF，不声明运行了产品代码 review-agent 门。
后续每个实现小任务必须显式调用官方只读 review-agent，批次组合审查后按 C++ unit/integration/process → Python wrapper checks 执行。

| Coverage lane | 本轮核对与剩余边界 |
|---|---|
| Production | requester 私钥与 issuer 的实际源码入口；独立 authority 仍 PLANNED |
| Call sites / bindings | 原 16 个维护调用方置于 R11-B8，保留既有兼容/迁移记录；本轮不改调用方 |
| Tests / oracle | R11 各卡登记 C++ process 结果和反例；具体新 selector 由编码前冻结，不伪造已执行命令 |
| Build / dependency | 使用真实 requester/provider 目标名称；no-Python 和依赖闭包置于 N5；无 native build |
| Lifecycle / migration | 同链 stream、continuation、recovery、replacement、cleanup 先于调用方迁移；保留 T016 全体验收 |

## Validation

- `verify-spec-kit-sync.py --require-entrypoints`：PASS，11/11 entrypoints 同步。
- Context Mode project/active health 起始 PASS；一次缺少 relevance sort 的检索被 guard 拒绝，改用仓库权威文件，无产品失败或资格含义。
- 文档校验、双 PDF 构建和最终检查结果在本文件末尾登记。
- Python-only native assertions、CLI/help/check-only、同进程 fixture 均不允许关闭跨进程任务。

## Miss Retrospective

- static：发现并修正旧 authority 例外与真实进程测试后置规则；新顺序直接登记 tasks，避免仅聊天声明。
- compile-link：产品 NOT_RUN；文档 XeLaTeX 构建单独记录，不计 native 证据。
- runtime-test：NOT_RUN；原有定向 PASS 范围不扩大。
- unobserved：R11-B1 至 B9 的产品验收全部保留开放；当前设计快照不因目标重排刷新为新实现。

## Final Documentation Checks

PASS：新契约/证据的相对链接均存在；R11-B1→B9 顺序无环；与本轮开始前比较，原有 checkbox 行逐行一致；`git diff --check` 通过；workflow 同步再次 PASS 11/11。
`python3 Design/build.py` 成功：current 91 页、target 97 页，最后一遍日志无 Warning、Overfull 或 Missing character；`design_state.verify_provenance` 通过，两份 PDF 与本次递归输入摘要一致。人工查看目标新增章节第 83–84 页。
本次只验证文档构建身份与变更内容，不运行会要求全仓库重新冻结源码的完整 `Design/verify.py`；既有 current/target 源码快照及 API 清单保持原基线，不宣称该历史快照等于最新 R10 源码。无产品 API 变化，未重建 API 清单。
文档单元 CLOSED_FOR_VALIDATION；R11 产品任务仍 NOT_STARTED，T016 NOT_RUN。
