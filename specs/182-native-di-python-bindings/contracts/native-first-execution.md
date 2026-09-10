# Native-First Execution Order

**Revision**: 1 | **Date**: 2026-09-10 | **Status**: PLANNED

用户确认的剩余执行顺序；覆盖旧文档中“所有真实跨进程用例推迟到 T016”及
“生产 requester 进程内持有 artifact authority 私钥”的规定。保留原 17 个父任务、
FR/SC/PO 和有效历史证据，不重做 Spec182，不将本轮设计修订计为产品行为通过。
本文件定义剩余阶段与新卡；[tasks.md](../tasks.md#execution-progress) 是状态唯一入口。

## Stage Order

| Stage | Required exit before advancing | Units / parent ownership |
| --- | --- | --- |
| N1 Independent artifact authority | authority 在 requester 之外持有签发私钥与受保护工件密钥；C++ authenticated grant 正反例通过 | R11-B1 / T005 |
| N2 Cross-process unary | C++ `DI_NativeRequester` → Core → C++ `di-native-provider`，实际 ACK、Selection、handler 执行及最终 Response 和数值 oracle 通过 | R11-B2 / T010 |
| N3 Same-chain stateful behavior | 沿 N2 原生生产链依次完成 stream、continuation、recovery、replacement、cleanup 正反例 | R11-B3--B7 / T010,T011,T009 |
| N4 Maintained caller migration | 16 个已登记旧调用方逐个有 native 路由和实际行为证据；Python 只作薄封装/兼容入口/外部编排 | R11-B8 / T012,T013 |
| N5 Closure and qualification | no-Python 与依赖闭包工具、T015 收敛、T016 完整资格，最后 T017 交接 | R11-B9 / T014--T017 |

顺序为 N1 → N2 → N3 → N4 → N5。下一项为 R11-B1，不再以一次全仓库扫描代替
该明确缺口的修复。保留 R10-B83/B84 的 C++ owner/name 修复；局部 PASS 不能替代 N2。
N3 是能力阶段，不是一次吞并五种状态行为的大批次。每张卡形成稳定出口即进行批末验证。

## Native Validation Order

每小任务：C++ production code → 独立官方 review-agent 只读静态门 → 同批下一任务。
整批逻辑/流程审查后：**C++ unit/integration/process tests → Python wrapper checks**。
先完成对应 C++ 生产逻辑与行为证明，再验证 Python 对它的参数、异常、寿命和转发。
五 lane、review trace、Closure decision 和四类 miss retrospective 复用共享
[batch contract](../../../skills/speckit-code-design/references/batch-quality-gates.md)。

原生行为的断言主体、fixture/driver 与 oracle 必须是 C++，直接调用具名生产 target。
Python 可以搭建外部网络或启动 C++ executable，但不得计算 planner/成功判据/恢复状态。
Python 测试数量、py_compile、CLI help、check-only、链接烟测、fake ACK/预置 Response、
同进程真实 Provider 都不能推进 N2/N3、native 父任务或 T016 状态。
批末定向 process tests 是开发验收，必须在迁移调用方之前执行；T016 仍负责最后的
完整同源 C++ unit → integration/process → MiniNDN/no-Python → Python wrapper suites。
没有重跑的新源码不能继承不兼容旧二进制的 PASS；有效未受影响证据按原范围复用。

## Independent Authority Boundary

- **Owner**: 复用 `NativeArtifactGrantIssuer`/现有 policy、grant wire、签名和 recipient
  encryption；部署到独立 C++ authority 进程。独立的不只是类名或 identity 字符串。
- **Requester**: 保留自己的签名私钥、authority 公钥/信任锚和寻址配置；不得读取、
  注入、继承或在配置中要求 authority 签发私钥。不得构造本地 issuer 为自己补发 grant。
- **Authority**: 持有 issuer policy、签发私钥和工件密钥，独立核对已认证 requester、
  provider recipient、request/attempt、plan/grant-view/model digest、epoch、purpose、
  residency 和有效期；返回既有签名与 recipient encryption 保护的 grant。
- **Transport**: 用既有 Core signed application Data/publication/fetch 能力连接 requester
  grant client 与 authority；不创建平行 DI 协作协议、不改 Request/ACK/Selection/Response。
  R11-B1 编码前冻结 transport adapter 的实际符号/名称、配置 schema 兼容错误、timeout/
  cancel、Face/executor 生命周期及 target/source closure；不得以待选端口直接开始编码。
- **Isolation proof**: 独立 PID 只是必要条件。验收使用不同密钥目录及权限/隔离环境，
  验证 requester 无 authority 私钥文件访问和继承 FD；不在日志输出密钥内容。
  requester 配置 authority 私钥或回退到进程内 issuer 必须被生产入口拒绝。
- **Failure**: authority 不可达/拒绝/超时、错签名/recipient/epoch/绑定必须 fail-closed；
  不产生可执行 Selection，不激活受保护工件，不恢复 plaintext 或本地自签 fallback。
  已有 Core Controller 权限撤销保持；本次不顺带重设计独立工件撤销协议。

## Dispatch Cards

所有卡均为待执行实现/验证义务。测试 target/selector 的新名称在卡片实现前登记为
PLANNED 并核对注册，不把尚不存在的测试名写作已执行命令。共同 Read：CD-004、
proof-design、既有 state contracts、R10-B82/B83/B84 evidence 和调用方。

| Unit | Depends | Read / Write boundary | Required result and independent counterexample |
| --- | --- | --- | --- |
| R11-B1 Independent Authority | T001 有效设计/依赖关闭；T005-A/B 现有实现作输入 | `examples/DI_NativeRequester.cpp`、NativeArtifactPolicyAuthority/NativeAuthenticatedGrantClient、生产 C++ authority 入口和 Waf target；C++ grant tests/config | 冻结端口后移除 requester 私钥/本地 issuer；独立进程签发、真实 grant 验证成功；错误 recipient/签名、私钥配置、authority 拒绝/不可达失败且无本地 fallback |
| R11-B2 Native Unary Process | R11-B1；T008/T009/T010 已有实现作输入 | `DI_NativeRequester`、`examples/DI_NativeProviderExecutable.cpp`、NativeInferenceClient/ProviderHost、C++ process driver/oracle 和测试注册 | requester、authority、Provider 为独立进程；真实 protected model request 经 ACK→Selection→handler→Response，结果与冻结输入/数值 oracle 相符；停止 Provider/拒绝授权不得伪成功 |
| R11-B3 Native Stream Process | R11-B2 | 同一 CLI/library/Provider stream 入口、C++ stream driver/oracle | 真实有序 stream 和唯一 final；错 generation、gap/timeout、重复/晚到事件按契约拒绝或收束；不能用直接 callback 注入替代线传输 |
| R11-B4 Native Continuation Process | R11-B3 | CLI conversation config、NativeConversationCoordinator、receipt/control/journal、C++ driver | 两轮 FULL_CONTEXT→APPEND_DELTA 经真实 receipt/COMMIT/FINALIZE；冻结 transcript/lineage；错 parent 被拒绝，不将 journal 存在当成功 |
| R11-B5 Native Recovery Process | R11-B4 | requester/journal/Provider state recovery 与 C++ kill/restart driver | 冻结支持的中断点，重启后安全恢复或明确拒绝；不得重复提交 prefix 或把 requester journal 当作 Provider KV 可用证明 |
| R11-B6 Native Replacement Process | R11-B5 | 原生重新规划、fencing、role map/receipt 与第二个 Provider | 真正切换独立备用 Provider，successor 结果/状态正确；旧 attempt/旧 Provider 晚到数据不能提交，无候选时单一失败终态 |
| R11-B7 Native Cleanup Process | R11-B6 | NativeInferenceClient/ProviderHost close、secret lease、Face/io_context/worker 生命周期与 C++ process tests | 成功、取消、超时、失败/替换后清理与排空；共享第二服务仍可用，无残留进程/回调悬空；C++ fixture 显式拥有依赖或 join/drain，不改生产语义掩盖测试竞态 |
| R11-B8 Maintained Callers | R11-B7；对应 T012 原生 ABI | 原 16 caller 清单、同库 facade、兼容入口和 T013 legacy 清退 | 按 caller group 分成有独立出口的子批次；先对照已通过 C++ 行为，再做 wrapper 与真实入口检查；逐项记录 native entry、结果/错误/取消及旧路径零使用；计数本身不关闭 T013 |
| R11-B8-G1 Native Generic Request Facade | R11-B7；T012-A/B | `APPClient.request_task` 与 public `InferenceClient.request_task` 的 generic inline/`REPO_REF` native compatibility route | C++ native owner 回归、模型/task/schema/options identity、结果句柄与 planner non-fallback 通过；stream/conversation、其余 15 callers、legacy zero-use 和资格仍留父卡 |
| R11-B8-G2 Native Generic Stream Facade | R11-B8-G1；R11-B3 stream contract | canonical `APPClient.request_streaming` 的 native stream compatibility route、回调/终态句柄 | C++ native owner 保持 stream 状态和结果权威；Python 只转换 bounded stream options、转发事件/完成/错误回调并保留取消/结果句柄；Python conversation、TOKEN_STREAMING adapter contract、其余 callers 和 legacy zero-use 仍留父卡 |
| R11-B9 Native Closure | R11-B8 | T014 isolation/manifest、安装依赖闭包、T015/16/17 原有卡 | 本地 no-Python/cold path/动态加载与必要反例，真实输出 hash/source/dependency 对齐；T015 通过后完成 T016，全部必要 PO 才可交付；SIF/Tiger 仍由实验机器负责 |

R11-B2 的 evidence 必须关联同一 request/attempt/plan 的 PID、签名身份、ACK、Selection、
handler 开始/结果及最终 Response，并记录实际二进制/共享库摘要和 oracle；只看到
`NATIVE_REQUEST_SUCCEEDED` 字符串或进程 exit 0 不足以通过。R11-B2 可用最小确定性
原生模型建立传输出口，但不得因此取消 T016 的 YOLO/Qwen/保护路径正式覆盖。
N5 前允许维护必要构建闭包以运行当前 C++ 批次；这不提前宣布全面依赖/no-Python 资格。

## Status Preservation

原 T005-A 的进程内 issuer 测试保留为组件证据，不再代表生产 authority 隔离。
T010/T011/T012/T013 保持现有 PARTIAL；新增 process 门满足后仍需满足父任务全部验收。
已有 caller 修改保留，不回滚、不据 Python 用例数量继续推进迁移关闭。
T015 在 N4 后做必要整体收敛审查；不得把“禁止现在再扫描”误解为取消最后的收敛门。
