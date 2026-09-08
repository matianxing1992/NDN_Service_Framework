# T008 Observed Offer Contract

## Status

2026-09-07 / PARTIAL。源码审计确认 NativeOfferAdmission::verify 从本地 policy 复制
freeBytes/backends/residency/acceptedRoles，并直接赋 preparationAccepted/executionAllowed。
当前仅有单测调用，没有消费真实 ACK payload，也未验证 offer 自身的 Ed25519 signature。
Core ACK provenance 与 offer policy-bound signature 都须保留，不能以其中一个代替另一个。

## Design and Source Review

新增 NativeObservedProviderOfferV3 是数据 DTO，没有 authenticated/admitted 字段或转为
planning view 的接口。decoder 复用 nativeParseJson/nativeCanonicalJson/nativePlanningDigest，
处理既有 V3 wire，不另建协议或 JSON 库。保留 topology/resource/residency 明细，
hasModel 不代表 exact reuse，CPU empty topology 不制造 GPU 或 RAM 观测。
必填字段先检查，默认字段按 SDK dataclass 补齐；输入整数成本保持整数摘要身份，
native double 仅用于读出的数值。未知字段、重复 key、非 canonical wire、非法计数及
资源越过 topology 被拒绝。源码检查先于构建；不改变既有 DTO ABI。

真实 Python SDK authoring script 生成六个固定 wire/digest/topology digest：CPU、CUDA
资源/loaded proof、整数成本、缺省字段、exact disposition、REJECT。签名是明确的 fixture
占位值，这些向量仅检验解码，不证明密码学验证或 admission。

## Validation

raw run：`.codex-tmp/spec182-t008-observed-offer-r1/`。
`build.log`：复用上一轮新 ABI consumer 树，只增加新类型/源文件，无既有 layout 修改；
system PATH、已配置的 system g++/binutils 与冻结依赖闭包，Waf unit-tests -j4 PASS（14.39s）。
`focused.log`：Spec182ObservedOffer、Spec182CanonicalJson、Spec182OfferAdmission 三个 suite，
20/20 cases、2154/2154 assertions PASS。原 admission suite 仅回归其既有行为，不证明该缺口修复。
六个真实 SDK vectors 的摘要一致；错误字段、缺失 required、负数/bool 计数、资源越界/
重复、无效 disposition/lease/时间区间、非 canonical/重复 JSON key/超限 wire 被拒绝。
本次没有全量回归、网络集成或正式资格验收。
T008-B 保持 PARTIAL；下一步将真实 Core ACK 与 policy-bound Ed25519 verification 接入，
移除 policy 伪观测路径，并让 planner 使用已认证的完整 device/residency 数据。
Context Mode project health exit 5 / NO_REAL_SESSION_EVENTS；使用仓库与 CodeGraph 回退。
