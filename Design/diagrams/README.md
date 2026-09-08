# 图解源码与证据入口

两份 PDF 的图解章各包含 G1--G9。使用 TikZ 矢量图，源文件直接参与现有 XeLaTeX 构建，不依赖截图或额外浏览器渲染环境。

| ID | 视图与问题 | 源码/契约入口 |
| --- | --- | --- |
| G1 | 模块关系与业务所有权 | docs/architecture.md；docs/ndnsf-core-app-boundary.md；四模块正文 |
| G2 | Core 角色组合与持有 | ndn-service-framework/ServiceContainer.hpp 的 addUser/addProvider/addController；三个角色的 .hpp |
| G3 | DI Client、Provider、handle、operation | NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp/.cpp；NativeInferenceProvider.hpp/.cpp |
| G4 | Repo helper 与存储/网络适配 | NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoClient.hpp、RepoNode.hpp、RepoCore.hpp |
| G5 | UAV 容器与飞控/媒体所有权 | NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp；ground-station/GroundStationServiceContainer.inc.hpp |
| G6 | Core 发现、选择、执行和验证 | ndn-service-framework/ServiceUser.cpp、ServiceProvider.cpp；Core 普通调用正文 |
| G7 CURRENT | DI 默认链实际终点 | NativeInferenceClient.cpp 的 request、dispatchOperation、markTerminal、cancelOperation、result |
| G7 PLANNED | DI 准备到唯一终态 | target-roadmap.tex TG-02/TG-03；Spec182 runtime/requester 契约 |
| G8 | Repo 元数据与完整对象 | Repo 控制/数据面、制品正文与 AC-10--AC-13；具体入口支持范围保留 |
| G9 | UAV 服务响应与飞行完成 | DroneServiceContainer、FlightControllerBackend、GroundStationServiceContainer；AC-19/AC-20 |

`current-diagrams.tex` 与 `target-diagrams.tex` 各自选择图文件。G7 当前与目标使用不同源文件，禁止互相覆盖；共享图仅表达两侧都成立且目标保留的关系，不自动同步最新新增行为到冻结目标。

对象持有须有成员/构造依据，标明值成员、unique_ptr、shared_ptr 或借用；调用不等于所有权。时序 lifeline 不表示线程，设备异步回报不假定固定顺序。逻辑阶段与代码枚举分开解释。

所有 TeX 纳入 build-provenance 的递归摘要。修改后构建双 PDF、运行既有校验并逐页查看两侧新增图；机器门不能证明语义正确。新增图登记源码/契约入口和 CURRENT/PLANNED 范围。
