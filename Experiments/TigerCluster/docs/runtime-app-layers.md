# Stable Runtime SIF And External Applications

## Decision And Status

2026-09-08 用户要求：SIF 固定 NDNSF/Repo/NDN 依赖，频繁变化的 DI/UAV 应用
放在镜像外，减少编译和镜像重建。此方案 **ACCEPTED / IMPLEMENTATION_IN_PROGRESS**。
Spec183 的旧“九个原生产物全部打入完整应用 SIF”是迁移前实现，不再作为目标。
基础层源码选择和构建入口已开始实现；现有正式 YOLO launcher 尚未完成分层发布接线。

本机采用单阶段 `library-runtime.def.in`，让基础 SIF 同时保留匹配的编译器和
开发头文件，作为本地应用 SDK；这些稳定工具不是 DI/UAV 应用。这样不需要
同时保留两份解包后的完整镜像。`build-external-yolo.py` 只在本机通过该 SIF
编译应用，以 base SHA、构建入口和编译参数隔离缓存；原生源码或ABI变化须重建受影响程序。
外部输出包含应用二进制、Python 源码和显式 base 绑定的 application manifest；
`BUILT_APPLICATION_CANDIDATE` 不代表 ABI/import、MiniNDN 或 Tiger 资格通过。
正式 launcher 的显式分层挂载、独立 app 清单和运输闭包已接入；正式资格回执
迁移和完整组合验收仍需完成。打包文件遗漏或权限修正可使用
`build-external-yolo.py --reuse-application <已验证应用目录>`，但必须保持同一
source seal、source revision、base SHA 和编译参数；保留原编译器构建身份，
重新冻结应用清单，不执行编译。源码变化不能使用这个快捷入口。
源码变化时可使用 `--build-cache-from <上次已验证应用目录>`：默认核对基础SHA、
编译参数和缓存所有权，仍执行configure与Waf增量依赖检查。构建成功后缓存
跟随新应用的buildKey，下一次引用最新应用即可。Python启动器改动已有实证：
configure6.728s、Waf0.837s、三原生二进制哈希不变，基础SIF不重建。
外置应用的目录也冻结为0555；宿主编排禁写字节码，避免导入产生未登记缓存。
固定注册的YOLO输入图片也是应用运行资产：打包时从完整源码封存复制到
`repo/tests/fixtures/spec180/yolo26n/fixed-fixture.ppm`，由应用清单与参考加载器
校验；只补齐漏包文件可复用同一源码/基础SIF的已验证应用，不重编原生程序。

基础库自身只有 Python 改动时，使用
`adapters/slurm-apptainer/scripts/repack-base-python.py render`，传入原基础SIF
及其SHA、原/新完整源码封存和独占输出目录。它只接受现有 ndnsf/py_repoclient
包内 `.py` 文件变动；依赖、原生源码、构建文件或文件集合改变均拒绝。
保留原源码准备参数，包括既有 `--derive-ndn-svs-version`，不能靠忽略封存
差异来复用。新镜像内部核对旧源码封存、已安装Python原字节、六个原生产物与
链接闭包，再更新Python、wheel RECORD、源码封存及基础运行记录；不运行编译器。
该入口只证明 Python 重封装边界，不能代替新组合的 MiniNDN/GPU 验收。

重封装仍生成新的基础SIF身份。外部应用只有在原生ABI及基础相关源码保持一致、
原程序字节再次验证之后才能重新绑定；禁止直接更改旧应用清单中的base摘要。
运行时不得通过宿主库覆盖旧SIF来声称镜像已修复。
上述Python重封装可显式追加 `--python-repacked-base`：构建器从新SIF内部读取
父SIF和源码封存绑定，验证旧app及其缓存，再运行新SIF的基础验证、configure和
Waf依赖检查。该模式不跳过构建检查，也不允许原生源码改动绕过重编译。

MiniNDN 在本机系统 Python 环境中编排网络 namespace，再通过 Apptainer
启动 NFD 和应用子进程。不要因容器没有 Mininet 而重建基础镜像；本机旧
`minindn-venv` 也不能代替已验证的系统环境。入口 `--help` 不触发延迟导入，
必须另外检查 `NDNSF_DI_Yolo2x2_Minindn` 与 `NDNSF_NewAPI_Minindn_Perf` 的
真实加载。外置包包含这些封装源码，实际挂载/命令仍须按分层方案接线。

[C++ NDN 诊断](ndn-smoke.md) 的 Tiger 作业 209981 已验证：同一历史 SIF 加
外部只读二进制可在两节点实际通信。它支持这个部署方向，但未验证 DI/UAV 的
所有 C++/Python ABI、鉴权或 GPU 路径。UAV 这里只定义可复用边界，不扩展
Spec183 的 YOLO 实施范围，也不启动 Spec182。

## Ownership Boundary

| 层 | 内容与责任 | 何时改变 |
| --- | --- | --- |
| Base runtime SIF | OS、glibc/libstdc++、系统 Boost 1.71、ndn-cxx、NFD/ndn-tools、锁定 NDN-SVS、NAC-ABE、NDNSD、NDNSF Core、NDNSF-Repo 的通用库/客户端绑定；Python、通用依赖及 ORT/CUDA 用户态运行库（使用时） | 基础组件源码、ABI、依赖版本或运行环境改变 |
| External application bundle | NDNSF-DI、YOLO 与以后 UAV 的程序、Python 包、应用专属 `.so`/扩展和配置；`di-native-provider`、fault provider、实验 Controller 等可执行程序 | 对应应用或适配器改变，独立构建/冻结 |
| External experiment assets | 模型、输入、oracle、harness/profile；私钥另用每角色私有挂载，结果另用可写目录 | 按既有内容清单和 run 身份更新 |

按实际链接与 API 边界分类，不按整个源码仓库或目录整体打包。Repo 的通用库
和 `py_repoclient` 属于 base；Repo 服务程序/实验逻辑属于 app。NDNSF 的通用
`ndnsf._ndnsf` 绑定随 Core 固定；DI 自有扩展属于 app。如果所谓“应用改动”
实际修改 Core/Repo API、通用绑定或基础库，仍须更新 base 并重建受影响消费者。
不能仅把基础源码移进 app 清单来绕开这一规则。

## Build And Execution

1. 本实验机以锁定源码构建一次可复用 base SIF，或选择已经满足当前基础锁和
   闭包要求的 base；历史镜像不因文件存在自动合格。基础依赖 ABI 不变时复用。
2. 在本机匹配该 base 的容器构建环境/SDK 中编译 app，输出独立安装目录。
   SDK 可包含头文件、编译器和静态构建工具；它由同一基础锁派生并记录摘要，
   不要求运行 SIF 安装编译器，也不在 Tiger 编译。复用现有 builder，
   不另建镜像工厂。最多 `-j2`，同一构建树不并行启动构建。
3. 构建缓存按 base/SDK/toolchain/flags/dependency-lock 分隔。键不变时仅编译
   受影响目标及其消费者；纯 Python 改动只重新冻结包。构建键改变时重建受影响
   消费者，不能复用不明来源对象文件。外部包不得携带宿主 venv 或替代基础库。
4. 生成新的不可变 app bundle，明确记录源文件/产物摘要、构建参数、编译器、
   SDK 摘要、所需 base runtimeId/SIF hash、入口、完整 DSO 与 Python 闭包。
   执行前绑定到固定只读 `/app`，harness `/bundle:ro`，模型独立只读，输出可写。
   不从活跃工作树运行，不覆盖 `/opt/ndnsf-di/current` 或镜像 site-packages。
5. 在实际 `base + app` 组合里检查 ldd/RPATH/SONAME、Python ABI/import 与真实
   entrypoint。app 的 RPATH 只允许清单内 app 库及 base 库；通用基础库不得被
   同名 app 库遮蔽。CUDA 驱动由 compute 节点通过受控 `--nv` 提供并实测，
   不从开发宿主打包驱动。正确加载后再做受影响的请求/安全/数值验证。
6. Tiger 校验并运行同一 base 和同一 app 包。每次新 allocation 校验实际环境、
   挂载及两者身份；大 SIF 缓存命中不重复上传。变化 app 只传新包。

## Identity And Evidence Reuse

沿用 I → R → E，不新建一套候选系统。分层布局显式版本为 `layered-v1`：

- I：基础源码/依赖/工具链/构建定义；排除仅应用文件变化。混合仓库须使用显式
  基础源文件闭包和内容 seal，完整仓库 revision 仍记 provenance，不能简单
  因 README/DI commit 改变就重编基础库，也不能漏掉被基础构建消费的文件。
- R：I、base SIF 和基础 native/library manifest。
- E：R、appManifest 摘要、harness/effective profile、模型/input/oracle/安全契约。
  appManifest 绑定 app 源码、产物、SDK 和 required R，不含 E 自身，避免循环。
- ResolvedRun：实际挂载、节点、GPU、run ID；GateReceipt 明确绑定其适用 R 或
  完整 E。基础验收可以复用，应用执行 PASS 不能跨 app 摘要借用。

| 改动 | 编译/运输 | 最小必要验证 |
| --- | --- | --- |
| 文档，不改变执行输入 | 无 | 文档一致性 |
| app Python/配置 | 新 app/harness 包，无 C++ 或 SIF 重建 | import/入口及受影响逻辑；网络/权限变化补相应集成/MiniNDN |
| app C++/自有扩展 | 受影响 app 目标增量构建，SIF 不变 | 在同一 base 验证 DSO/Python ABI 和受影响行为 |
| Core/Repo 通用绑定或基础 ABI/依赖 | 新 base，重建受影响 app 消费者 | 相关 unit/integration/MiniNDN，精确新组合本地验证，再受影响 Tiger 门 |
| 模型/input/oracle | 无 native/SIF 构建；只更新变化内容 | 对应数值/设备/网络门 |
| 新 allocation，内容未变 | 内容寻址复用，不重编/重复上传 | 节点/GPU/Apptainer/挂载/身份/清理；执行登记的该次实验 |

“完整候选/精确运行环境”指 `base SIF + app bundle + execution inputs` 的组合，
不要求这些字节都烘焙进同一个 SIF。正式 YOLO 顺序仍为生产审计 → unit →
integration → MiniNDN → exact-composition local → single-node GPU → two-node。
同一通过的检查只在对应输入或环境变化时重跑；失败后只重跑修复所影响的门。

## Migration And Rollback

现有 definition 的 DI 安装、九产物 preflight 与旧 R 摘要语义仍在代码中。
T002/T004/T011 将在原 owner 中拆分 source seal、native manifests、预检与挂载；
T007 审计真实消费链，旧 receipt 不改写、不自动升级。新布局的 schema/版本
必须显式拒绝未识别的旧/新混搭。旧 `legacy-monolithic` 仅保留冻结历史证据与
已知合格组合的回退入口；新的 Spec183 发布只采用 layered-v1，迁移资格通过后
移除 Spec183 的旧创建路径，不改动其他仍有独立契约的历史 workload。

回退选择已验证的完整 `(R, appManifest, E)` 组合，不拼接“最新 app + 旧库”。
本次不重封已有交付锁，不重建 SIF，不提交额外 Slurm 实验。下一步先落实
外部 app manifest 与 base ABI 验证、原 builder/worker 接线，然后复用已跑通
的 NFD/TCP 配置验证一个最小 NDNSF 服务，再推进 YOLO。
