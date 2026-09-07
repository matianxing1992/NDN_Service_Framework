# T010-A Lifecycle Audit and Repair

## Scope and Source

2026-09-07，继续用户目标：核对实际进度、修正问题、完成全部 Spec182。
读取基线 f3cb2d9e，已有未提交 NativeInferenceClient.hpp/.cpp 串行执行器切片，
本单元在其上修正并验证。两份 native dependency/generation 设计的预存 diff 不属于本单元。
Context Mode project health rc5、active rc4，使用仓库指针、tasks、契约与原始证据回退；
CodeGraph 返回的 .codex-tmp staging/compare 副本剔除，只以生产文件核对。
implementation hook 配置无 before_implement/after_implement 条目。

## Actual Progress

执行表 21/36 子卡 DONE，父任务 5/17；未独立重新验收全部已勾选项。
T006--009 子卡虽已 DONE，父任务仍可能缺集成用例编写、生产接线等本地交付义务。
T008/T009 evidence 多处把 I-file 编写交给 T016，与原 plan 的“实现期编写、T016 执行”
存在偏差；这些义务必须在 T015 前补齐，不能仅凭有效局部单测关闭全 Spec。
T010 未提交源码仍固定报 NATIVE_REQUEST_PIPELINE_NOT_READY，完整 model/input→Response 未实现。
当前单元不关闭 T010-A/B，也不把产品状态重置为 NOT_STARTED。

## Review and Change

- 原 publishEvent/observe 在调用线程同步调用 observer，慢回调可阻塞 cancel/close 或 Core 线程，
  与 CD-001 独立串行通知队列要求不符。改为单独队列，在 operation mutex 下按序入队，专用线程执行。
- 通知队列由 client 与 operation 共同持有，client close 后存活 handle 仍能重放终态。
  线程只持队列 State，不持 executor facade；最后 owner 释放时 stop/drain，避免空闲线程自持有泄漏。
- request 创建 operation 后 submit 抛异常原本直接逃逸；现在返回结构化失败 handle，原因码
  NATIVE_REQUEST_DISPATCH_FAILED，不宣称成功或保留永久 Pending。
- 测试使用真实 LocalMock ServiceUser + DummyClientFace，时钟/分派順序可控；
  test adapter 仅用于进入当前公开请求状态边界，不用合成成功替代完整请求。

## Checks

源码审查：client/handle/executor/notification 所有权、cancel/close、等待与重放及新测试。
GCC9.4 `/usr/bin/g++ -B/usr/bin`、GNU ld2.34；Waf cache 为 system Boost1.71、
NAC prefix nac-abe-integration-182/install、实际 NDN-SVS source/build pair、固定 ONNX/Rust archive。
没有 reconfigure 或并发启动另一个 Waf build；复用现有 build-nac182。

- `PATH=/usr/bin:/bin:/usr/sbin:/sbin:/home/tianxing/.local/node-v22.23.1/bin /usr/bin/python3 ./waf -o build-nac182 build --targets=unit-tests -j4 -v`：exit0。
- `timeout --kill-after=5s 120s ./build-nac182/unit-tests --run_test='Spec182ClientState/*' --report_level=detailed`：exit0，3/3、11 assertions。
- 同命令选择 `Spec182NativeInferenceClient/*`：exit0，既有 2/2。

原始命令/结果：`.codex-tmp/spec182-t010a-audit-r1/build.log`、client-state.log、client-existing.log。
新 suite/name 登入现有 case-manifest。慢观察者 case 检查 observer 阻塞时 cancel future 已返回，
再释放 observer；销毁 client 后检查存活 handle 终态重放和迟到 dispatch 不复活。
没有运行完整回归、integration 或 MiniNDN，未据此作资格验收结论。
文档校验首次误拒 T006-C evidence 中真实存在的目录链接，修正 validator 接受文件或目录，
Markdown 文件锚点检查保持；不改历史 evidence 链接或产品验收。随后 design validator 与 diff 检查 PASS。

## Remaining

T010-A 仍 PARTIAL：成功/取消竞争、主动 deadline、Core 回调 fencing、非公开测试端口收口、
通知容量/溢出和回调自销毁验证仍需随真实请求接线完成。T010-B 需贯通 Core BeginCollaboration、
prepare/admit、split/place、seal/grant、commit 与 Response；T010-C 验证 stream/replacement/final。
保留 T011--017 全部目标和验收门，不能把本生命周期修正当成完整请求交付。
