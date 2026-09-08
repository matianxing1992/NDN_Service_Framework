# T008 Core Offer Admission

## Status and Design

PARTIAL。以真实 Core AckSelectionCandidate 替换 NativeAckEvidence caller DTO，删除
policy capabilities。构造时冻结既有 spec180-provider-offer-trust-v1 policy、candidate digest
和公钥注册表，校验 Ed25519 类型与 raw-public-key SHA256 key ID。verify 先检查 Core
provenance，再解码 ACK payload，验证 Provider/service/request/attempt/model/graph、
KeyLocator、状态/无 reservation、有效期覆盖 deadline 和 offer Ed25519 signature。
NativeAdmittedOfferV3 仅 admission 可构造，保留完整观测；不产生 lease 或 planner 默认资源。
真实订阅接线、planner 消费及网络验收仍待完成。

## Authoring Failure r1

author-signed-offer-oracle.py 导入 app_sdk.provider 时由 package __init__ 进入
artifact_deployment，缺少 py_repoclient 搜索路径。首边界为 fixture authoring 的 Python
环境导入，不是签名或协议结果。补齐仓库 Python 路径后再重试；不改生产 package。
raw run：`.codex-tmp/spec182-t008-core-offer-r1/`。

## Source Review and r2

补齐 NDNSF-DistributedRepo/pythonWrapper 后，真实 ProviderOfferTrustVerifier 校验六组
Ed25519 fixture 签名 PASS；使用公开固定测试 seed，只有公钥和签名写入 fixture。
新 gate 从 Core candidate 读取状态与 payload，不接受单独 caller trust DTO。KeyLocator
使用 NDN component prefix，避免 fixture 与 fixture-foreign 字符前缀混淆；signature
限制 64-byte Ed25519 与 canonical base64。观测类型无可调用的公开认证构造器。
旧 15-case policy 伪观测测试替换为真实签名/篡改/身份/请求/时间/公钥替换测试。
这些 Core candidate provenance 仍是明确的单测 fixture，真实订阅验收留 T016。
raw r2：`.codex-tmp/spec182-t008-core-offer-r2/`；新类型布局使用新 ABI consumer build 目录。

Context Mode stats 可调用，project health 仍为 NO_REAL_SESSION_EVENTS；继续使用仓库
权威记录与 CodeGraph。design-validation.json 的 errors 为空，未将该结构检查视作运行验收。

## Focused PASS

r2 configure.log PASS（5.302s）；build.log PASS（304.124s），Waf 新目录 unit-tests -j4，
system g++ -B/usr/bin、冻结 NAC-ABE/SVS/ONNX/ORT/Rust 闭包，日志含真实编译/链接命令。
vmstat.log 丢弃首行后 si/so 均为 0；仅为短样本，不声称全程峰值或并行度加速比。
focused.log：Spec182OfferAdmission、Spec182ObservedOffer、Spec182CanonicalJson，
10/10 cases、2158/2158 assertions PASS。六组 real SDK signed offer 与 native verify 一致，
覆盖 CPU/CUDA/整数成本/默认字段/exact/REJECT；REJECT 可认证但不因此获得执行权。
篡改字段、错 key ID、公钥替换、非法 signature、缺失/错 Core provenance、KeyLocator
component-prefix 混淆、外层状态不符、跨请求/attempt/service/model/graph 与时效不符均拒绝。
原 15-case policy 伪能力 suite 被上述真实签名 fixture suite 替换；不沿用旧测试计数。

CodeGraph 核对 ServiceUser.cpp::makeAckAuthenticationEvidence：无 packet 返回空证据，
真实订阅路径携带已验证 packet 的 signer/locator/wire digest。此次不改 Core 验证器。
同源 7970–7993 行在 ACK collection 前读取 ControllerVersion hint 并检查 Controller
transition 与 Provider permission；DI 不再验证手填 controllerVersion 字符串。实际 requester
必须经这个 Core 路径保留调用上下文，不能直接用 fixture 构造 candidate 作为上线入口。
新增 admission 方法尚未由 NativeInferenceClient 主链调用，也未构建完整
NativeProviderPlanningView；T008-B/T010 保持 PARTIAL，不能把测试 fixture 视为真实 NFD 验收。
