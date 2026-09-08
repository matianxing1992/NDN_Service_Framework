# T008 V3 Artifact Publication

## Design and Status

PARTIAL。旧 ensureArtifacts/ArtifactPort 只消费 NativePlacementProposal，阻断完整 V3
路径；且旧实现只核对返回角色覆盖与摘要格式，未比较选定工件，允许端口返回另一个合法
SHA-256。迁移到 model/candidate/V3 proposal/control，publication port 直接接收实际
candidate、完整选定 roles，复用 validateRoles 和 validateNativeAssembly。

发布前绑定 request/attempt/model/graph、未过期 context、完整 selected role/rank、唯一
Provider 分配与 offer digest 引用；返回值必须精确覆盖角色、匹配 inspected manifest 及
每个 selected artifact digest。请求身份由 control/model 覆盖端口自称字段；端口前后
检查单调 deadline/cancellation。requester 必须先用原始 prepared roles/admitted offers
校验 placement 可行性；canonical 对象发布不是 grant 或 Provider 执行授权。

## Static Review and Validation Plan

CodeGraph/call-site 检查确认调用方为 preparation、sealer 与 V3 placement 定向 fixtures；
尚未接通的 requester 没有生产 ensureArtifacts 调用。迁移现有 preparation 的正/负测试，
保留跨请求、空/重复角色、错误引用、缺端口、取消/过期检查；V3 SDK oracle 的 CPU、
GPU exact-reuse、多 rank 场景改为经真实 ensureArtifacts API 后再 seal/grantView。
新增合法 SHA 的外来工件拒绝，以及错误角色工件在 publication 前拒绝的断言。
既有单角色 sealer/grant acquisition fixture 改为调用 V3 publication API；旧 sealer
对照入口仍由后续迁移任务退休，不新增生产兼容回退。

ArtifactPort 函数签名属于 ABI 改动，使用全新 consumer 目录
`.codex-tmp/spec182-t008-v3-artifacts-r1/build`，system compiler/binutils、原冻结
NAC-ABE/SVS/ONNX/ORT/tokenizer 闭包，-j4 单构建。只运行必要构建和相关定向检查；
Python 扩展须在后续接线时按新 ABI 重建，旧扩展不能作为此次运行证据。

## Validation

r1 configure PASS（4.691s）、全新 -j4 build PASS（280.317s），focused exit 201：
57/58 cases PASS，PreparedArtifactsReachGrantAcquisitionWithoutBackfill 在 publication
前触发 DI_NATIVE_ROLE_BINDING_MISMATCH。旧 sealer fixture 填 requiredDeviceMemoryMb=1，
真实 NativeQwenLayerSplit 候选为 1 byte weights + 1 GiB workspace + 两项 512 MiB，
safetyMargin=1.10，最低 2253 MiB。修正 fixture 并保留选定角色供后续 sealer 使用；
不降低产品校验。r1 原始日志保持；r2 独立日志目录复用此新 ABI 树增量重验。
这不是网络/协议失败，新的三场景 V3 publication 与所有 preparation 定向检查已通过。

## Focused PASS

r2 build.log PASS（16.139s，只有 fixture 修正，无新 ABI 变更），focused.log 中
Spec182V3Placement、Spec182Preparation、Spec182OfferAdmission、Spec182NativePlanning、
Spec182PlanSealer、Spec182GrantClient、Spec182NativeInferenceClient：58/58 cases、
553/553 assertions PASS。覆盖 selected role 原样传到 publication、合法 SHA 的错误工件
拒绝、端口请求身份覆盖、CPU/GPU exact-reuse/multi-rank 的既有 SDK core digest 对照。
r1 ldd.log 无缺失依赖；vmstat-start/middle/end.log 启动一次 so=74620 KiB/s，中途一次
so=356 KiB/s，末段 so=0/少量 si；只作短采样记录，不证明峰值或比较加速比。
design-validation.json errors=[]，git diff --check PASS。未执行全量回归或网络资格验收。

## Remaining Production Work

真实 catalog 认证/Repo publication owner、完整 execution dataflow/device binding 和
NativeInferenceClient Begin/ACK_CLOSED/Commit/response 主链仍待实现；T008-A 保持 PARTIAL。
Context Mode project health 非零，使用仓库与 CodeGraph 回退，不使用 timeline 作为进度权威。
