# API 查询入口

[Python 原生绑定映射](python-bindings.md) 另登记两个 pybind11 源文件的 Python 名称、C++ 目标/lambda 参数、默认值与调用策略；不把 C++ 签名当作 Python 参数列表。构造器 py::init 和动态导出仍须核对源文件。
映射含 830 项绑定操作（含 enum value 与属性，不全是函数）。其中 8 项属于 bindDeploymentControlMessage<T>(m, name) 的动态类名 helper，标记 see-source-owner，具体导出名须查其调用点，不伪造静态类名。

| 模块 | 完整声明参考 | 源文件数 | 声明条目数 |
|---|---|---:|---:|
| NDNSF Core / Python 绑定 | [Core](core-reference.md) | 43 | 5067 |
| NDNSF-Repo | [Repo](repo-reference.md) | 20 | 1335 |
| NDNSF-DI | [DI](di-reference.md) | 201 | 8141 |
| NDNSF-UAV | [UAV](uav-reference.md) | 31 | 2015 |

共 295 个规范源文件、16558 个声明条目，其中 5738 个函数声明/定义条目。其余含类型、数据字段、枚举值和别名；不是 16558 个公开调用 API。重载、构造器、protected 扩展点与应用内部接口分别登记，不做 ABI 稳定性承诺。

## 行为与签名如何对应

PDF 的 API 部分包含 23 组中文契约与 53 个准确签名示例，说明输入/输出、默认值、前置条件、处理流程、状态、安全与生命周期。完整重载及其他成员在本目录查询，原始注释保留英文；中文说明由人工维护。
[contract-map.json](contract-map.json) 将契约 AC 编号映射到 API ID，[inventory.json](inventory.json) 保存文件 SHA-256、源码行号、签名、可见性、原始说明及类别。
getter/同类重载可以共用一份语义；宏、继承展开和运行时动态导出未由语法清单自动证明。Python public-by-name 表示命名可见性，不能等同于包 __all__ 公开稳定性。

## 当前和目标

本目录完整声明参考描述当前源码。当前行为契约在 ../api-contracts.json，目标行为契约独立位于 ../target-api-contracts.json；各自渲染到 current-api.tex、target-api.tex。本轮 R1 一致。
后续目标新增接口可使用 planned_signatures，明确标 PLANNED；当前渲染器拒绝把计划接口写成实现。目标旧签名若要继续保留，使用目标契约中的明确声明和 Spec 差异记录，不能仅引用已变化的当前清单。
所有历史版本可从 Design 的 Git 提交恢复；禁止默认重新生成目标正文。

## 精度边界

这是源码声明与关键行为契约的完整子系统参考，不是每个辅助 getter 的独立教程，也不是逐行实现审计。未声明的异常/线程或网络资格不做推断。入口接线状态以正文和 active Spec 证据为准。
