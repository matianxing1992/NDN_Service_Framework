# Layered Runtime Delivery

## Decision and Status

2026-09-08 用户授权：稳定依赖留在 SIF，频繁变化的 DI/UAV 外置。
**ACCEPTED DESIGN / PLANNED TOOLING**。后续 Tiger 交付以基础 SIF 与外置应用包的
固定组合为候选；现有 complete-application SIF、启动器与历史证据不因此自动转换。
本修改不启动镜像构建、上传或集群作业；实验机器分工不变。

## Ownership

| Artifact | Content | Rebuild trigger |
| --- | --- | --- |
| Base SIF | OS/C++ runtime、ndn-cxx、NFD、NDN-SVS、NDNSD、NAC-ABE、NDNSF Core、稳定 NDNSF-Repo 组件，以及锁定的 ORT/ONNX/tokenizer/native dependencies | 所含库、工具链、ABI、配置或安全修复变化 |
| Application bundle | 原生 DI/UAV 程序、libndnsf-distributed-inference、应用 adapter、配置、启动器与必要可选 bindings | 对应应用及其真实依赖变化 |
| Model/artifact bundle | 模型、tokenizer 数据、canonical source、模型制品 | 模型及制品身份变化；单独锁定，不混入基础镜像 |
| Run evidence | 基础 SIF、应用、模型、profile 的组合身份和本次运行结果 | 每次运行独立生成 |

NDNSF-Repo 只有已锁定且稳定的运行组件属于基础层；应用特定 Repo 配置属于应用层。
某个依赖频繁变化时先调整归属和依赖锁，不能同时在内外放两份库再依赖搜索顺序选中。
本分层不改变 Core/DI/Repo/UAV 的业务职责，不把 DI 逻辑下沉到 Core。

## Build and Load Contract

1. 基础 runtime SIF 与对应 builder 来自同一锁定依赖闭包。builder 提供实际头文件、
   链接库、编译器和构建工具；runtime 不必携带工具链。记录二者身份与依赖匹配证据。
2. 应用源码在该 builder 内编译，输出到外置 staging。复用按 builder/依赖/配置身份
   隔离的构建缓存；只构建变更应用及受影响消费者。不能跨 ABI 复用缓存。
3. staging 打包为完整、不可变、内容寻址的应用版本；包含可执行文件、原生库、
   应用配置/launcher 和 manifest。运行时只读挂载该版本，禁止绑定活动源码工作树
   或可变 latest 目录充当资格候选。构建缓存不随应用交付。
4. 应用库使用明确的 RUNPATH/加载路径；记录实际 resolved libraries、hash 和 ABI。
   Core/Repo 等基础库只能来自指定基础 SIF；应用库来自指定应用包。Python bindings
   如启用，也必须在对应 builder 内按正确 SOABI 构建，并转发到同一原生 DI 库。
5. 新组合必须通过容器内依赖/符号/启动检查，再按实际变更执行规定验收。仅应用改动
   可复用未变 SIF，但旧组合的运行 PASS 不能直接赋予新应用。依赖或 ABI 变化时
   重建基础层与受影响应用，并重新核对组合；不能仅换 mount 绕过兼容检查。

外置不等于宿主机编译：host `.so`、host venv 不能成为容器运行依赖。
GPU 驱动仍由既有 Apptainer/NVIDIA 契约管理；基础层锁定的 ORT/CUDA 用户库与
节点驱动兼容性继续单独验证。无 Python 的 DI 进程验收要求不变。

## Composite Identity and Launch

扩展现有 source seal/build record/run record/profile 校验，复用 SHA-256 和已有
候选机制，不新建第二套发布入口。组合记录至少绑定：基础 SIF hash、对应 builder
及依赖锁身份、应用源码提交与 bundle/manifest hash、模型制品身份、launcher/profile
hash、实际只读 mounts 和 effective configuration。字段名由现有 schema owner
在实现时统一冻结；未知字段或不匹配组合须在外部副作用前拒绝。

启动器必须确实执行外置 bundle 中的 DI/UAV 程序；旧 SIF 内即使存在历史应用，
也不能经 PATH、PYTHONPATH 或动态加载回退到它。首次转换必须核对实际 executable、
loaded libraries 与绑定身份，并验证缺包、篡改、不匹配基础 SIF、错误加载路径拒绝。
单个 SIF hash 不再足以标识整个实验候选。更新 profile/mount 契约时同步对应验证器
和负例，未完成前禁止宣称旧 launcher 支持新的分层方式。

## Spec182 Delivery Mapping

- T002/T012：原生库及可选绑定边界保持既有职责，支持独立消费；不复制业务实现。
- T015：核对原生应用没有从环境偷取第二份 DI/Core 库，确认交付约束与默认入口一致。
- T017-A：交付 exact source、应用与基础依赖清单、对应 builder 内应用构建/打包方法、
  组合身份及启动器所需变更和负例；明确哪些工具已实现、哪些由实验机器接续。
- 外部实验 owner：实现/验证基础 SIF 与应用包构建、profile/launcher 组合校验及
  exact-pair 容器验收，再运行 Tiger。此责任列入 handoff，不借用本地 PASS。

原 Spec182 本地 T016 unit/integration/MiniNDN 门不变；外部 tooling/SIF/Tiger 记录
PLANNED/TRANSFERRED，必须有接续步骤，不把“方案已接受”记为“部署已完成”。

## Documentation Validation

FR-014、plan Delivery、T017-A、Tiger README/sif-build 与 packaging README 已同步。
定向新链接、关键边界与本次 diff 检查 PASS；完整
[design validator](../../../.codex-tmp/spec182-r1-b3-r3/design.json) PASS。
本记录只证明方案文档一致；未构建 SIF、应用 bundle 或运行 Tiger。
