# T003 V3 Strategy Interface

## Design and Status

PARTIAL。NativeInferenceClient 接收 NativePlacementStrategy 基类，但旧基类只有简化
snapshot/candidate 的 propose，完整 V3 proposeRoles 只在默认类上，无法用于真实注入。
改为基类必需 V3 虚接口，默认类 override；旧 propose 留在具体类作为待迁移 fixture
入口，不为自定义策略保留旧 DTO 回退，也不引入 Python trampoline 或 dynamic_cast。

## Static Review and Validation Plan

CodeGraph 与调用点检查确认当前唯一生产派生类为 NativePreSplitFirstPlacement，Python
绑定只注册原生类型与构造，不绑定旧 propose、没有 trampoline。requester 当前尚未调用
placement，故不能把接口修复当作主链完成。新增只实现 V3 的自定义原生策略 fixture，
通过 shared_ptr<const NativePlacementStrategy> 选择另一合法 Provider；仍由公共验证器
拒绝篡改 offer digest。原有 15 场景 SDK 对照改经基类调用默认实现。

虚表契约改变，使用全新 `.codex-tmp/spec182-t003-v3-interface-r1/build` 构建 Core/DI
与 unit-tests；system compiler/binutils、冻结依赖、-j4。现有 Python 扩展不得作为新 ABI
运行证据，绑定重建与真实请求验收仍由后续接线单元完成。仅做必要构建和相关定向检查。

## Validation

PASS for focused checks。r1 configure.log PASS（4.968s）、build.log PASS（310.850s），
全新 consumer 树，真实编译/链接命令由 Waf -v 保留；没有复用旧 ABI 对象。
focused.log：Spec182V3Placement、Spec182Preparation、Spec182OfferAdmission、
Spec182NativePlanning、Spec182PlanSealer、Spec182GrantClient、Spec182NativeInferenceClient，
58/58 cases、498/498 assertions PASS。client suite 只覆盖空 handle/缺 Core owner，
不证明真实请求推进；自定义策略的虚调用由新增 V3 fixture 覆盖。
ldd.log 无缺失库，NAC-ABE/SVS/ORT 实际加载路径与配置闭包一致。
vmstat-start/middle/end.log 保留短采样：启动阶段一次 so=6876 KiB/s，中途 si/so=0，
末段少量 si=4 KiB/s；未见这些样本内持续换页，不据此宣称全程内存峰值或速度对照。
design-validation.json errors=[]；git diff --check PASS。T003-C 保持 PARTIAL。
Context Mode project health 仍未提供真实会话捕获，使用仓库与 CodeGraph 回退。

## Next Production Boundary

追踪 ServiceUser.hpp:858/887 与 NativeRequestPreparation.hpp:78/102 后确认：Core 已有
BeginCollaboration 的 ACK_CLOSED callback 和绑定 closure digest 的 CommitCollaborationPlan；
DI 的 ensureArtifacts/ArtifactPort 仍接收 NativePlacementProposal，无法直接消费新的完整
角色集合。下一单元需将 publication 输入迁移到实际 candidate 与选定 V3 roles，保留
request/attempt/model/manifest、精确 role/rank/artifact cover 和取消/绝对 deadline 检查；
发布之前校验策略结果，禁止为调用旧端口重建简化 view/proposal。
随后才能连接 prepareInput→Begin→ACK_CLOSED→inspect/split/placement→ensure→seal/grant→Commit。
实际 catalog 认证、execution dataflow/device binding 及最终响应解码仍须由对应原生 owner
完成；不能以测试 port、接口存在或 Core 方法存在代替生产调用证据。
