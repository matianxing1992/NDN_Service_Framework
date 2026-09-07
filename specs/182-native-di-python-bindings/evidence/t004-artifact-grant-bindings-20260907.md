# T004 Artifact and Grant Binding Repair

## Status

2026-09-07 / PARTIAL。此轮修复 A8-01 的真实工件和 grant 输入缺失；完整 canonical
JSON、Selection wire、device/assembly/dataflow 仍未完成，不关闭 T004 或 T005。

## Implementation and Source Review

- NativeRequestPreparation::ensureArtifacts 在检查端口输出和精确角色覆盖后，从已验证的
  control/model 写入 requestId、attempt、modelDigest、graphDigest，覆盖端口伪报的上下文。
- NativePlanSealer::sealCore 强制接收 NativePlanSealingInputs：真实工件绑定、requester、
  protection epoch、原请求 wire expiry。移除按角色名合成 artifact digest 的逻辑。
- core 检查请求/模型/图绑定、精确角色覆盖、有效期和每个 Provider 仅一个 role。
  grantView 核对冻结 offer 和保护策略，并完整传递 acquire 所需的请求、manifest、epoch、expiry。
- 单测直接执行 ensureArtifacts → sealCore → grantView → acquire，中间不补字段。
  issuer/publication 是注入的测试端口，因此只证明输入链和拒绝边界，不证明实际密码学或网络。
- canonicalCore 仍是旧自定义摘要格式，encode 仍是旧片段。旧七字段字节断言保留为局部
  回归，测试注释明确它们不能充当生产 parser 或跨语言 canonical oracle。

## Validation

原始记录：`.codex-tmp/spec182-t004-bindings-r1/`（不入 Git）。

- configure.log：系统 compiler/binutils、Boost 1.71、既定 NAC-ABE/SVS/ONNX/Rust archive。
  公开 DTO 和 sealCore 签名改变，因此使用全新 build 目录重编译消费者。
- build.log：`python3 ./waf -o .codex-tmp/spec182-t004-bindings-r1/build build --targets=unit-tests -j4 -v`，
  exit 0，285.07 s，CPU 344%。time 报告的 1,622,504 KiB max RSS 不是四个进程同时占用的总峰值。
- focused.log：新 build/unit-tests，
  `--run_test=Spec182PlanSealer,Spec182NativePlanning,Spec182Preparation --report_level=detailed --log_level=message`，
  exit 0；33/33 cases、269/269 assertions；PlanSealer 12/12、142 assertions。
- 未执行集成、全量回归、MiniNDN、SIF 或 Tiger；这些结果不是 T016 资格验收。

## Placement Audit and Next Work

CodeGraph 对真实 NativePlanning.cpp:170 的查询确认 propose 先要求一个 offer 支持全部角色，
再把所有 role 分配给 eligible.front()；residency 排序使用摘要集合大小，未判断是否为目标工件。
这是 T003-C 的语义缺陷，不是新 sealer 拒绝规则导致的环境失败。旧 rank-one 用例通过
不足以关闭多角色契约，因此重开 T003-C 及依赖它的 DONE 状态。

Python sdk/placement.py 的 PlacementPlanCoreV3 与现有 Provider Selection 契约要求
每个执行 role 对应不同 Provider。此前 A8-01 中“多角色/rank 覆盖”应理解为跨 Provider
完整覆盖，不能解释成给单个 Provider 塞多个 role。下一步按逐角色 accepted/backend/budget
匹配、目标工件 residency 和 Provider 唯一性修复 placement，再完成完整 canonical wire。
