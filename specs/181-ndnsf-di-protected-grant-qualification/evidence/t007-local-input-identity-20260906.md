# Local Input Identity

**Status**: PASS (configured external input unit only; T007 BLOCK)
**Evidence layer**: executed (focused inventory/gate fixture boundary)

## First Boundary and RED R1

清单的启动环境摘要只绑定路径字符串，未绑定外部文件内容。单个
MiniNDN 案例虽然有部分输入核对，却不能证明三个案例与整张清单
使用同一组模型、配置和密钥引用。R1 的模型替换、目录新增文件、
映射引用的 key 替换与映射替换均进入测试禁止的 child 边界：
**4 failed / 43 deselected in 0.55 s**。没有执行资格子进程。

命令：`python3 -m pytest -q tests/python/test_spec180_local_gate.py -k external_input_drift`。
原始 `focused-red.log` 与 `source.patch` 保留在 ignored workspace
temporary directory 的 `spec181-local-input-identity-20260906-r1/`。

## Repair Contract

维护 inventory/gate 增加独立 inputIdentity 平面，保留启动配置摘要
原有语义。按显式环境枚举模型目录全部文件、输入配置/registry/
trust root、映射及引用的公私钥文件、受保护 authority key，记录
路径/解析路径/文件摘要，绝不记录私钥内容。未配置项显式标记，
实际案例仍由既有 validate_inputs 检查必需输入与密码学策略。

builder 在发现选择器前后核对；gate 在输出/child 之前、各网络
案例之前及结束后核对，变化使整体 UNQUALIFIED 并保留已有结果。
此平面不宣称覆盖运行中的恶意替换后恢复、完整系统依赖或 SIF。
选择器发现使用与后续执行相同的显式环境，不继承 ambient 环境
或额外注入 PYTHONPATH。T007 仍保持 BLOCK，正式矩阵尚未执行。

## Focused R2 and Fixture Failure R3

R2 三个维护测试文件 **66 passed in 5.88 s**，原四项拒绝通过；
CLI collection 与后续 Python child 使用显式环境。追加运行期间
输入变化、引用文件与密钥内容不泄漏检查后，R3 **3 failed / 69
passed in 7.68 s**：三个新测试遗漏 `json` import，尚未调用被测
输入 owner；修正测试导入后再验。两个真实 child 漂移/结果保留
用例已通过。R2/R3 日志及 R3 补丁保留，未运行正式资格。

## Focused GREEN R4

最终 **72 passed in 8.40 s**，命令：
`python3 -m pytest -q tests/python/test_spec180_local_gate.py tests/python/test_spec180_inventory.py tests/python/test_spec181_inventory_scope.py`。
R4 `focused.log` 与最终 `source.patch` 保留。

真实 fixture child 验证两种时机：Python 检查后变更输入，三项
网络案例均不启动；末项执行后变更输入，六项结果保留但整体为
UNQUALIFIED。两者均保存已执行项日志/退出/清理与未执行 ID，
不补造未运行子项。输入记录还验证受保护默认 key、映射引用、
目录 link 拒绝、缺引用/错误 map 拒绝以及秘密内容不出现在记录。
真实 CLI 的 C++ listing 与 Python collection 均消费同一显式环境。

清单新增必填 `inputIdentity`/`inputDigest`，实际生成和消费双方
均重算核对；旧清单缺字段直接拒绝，不把旧结果补字段晋升。
该证据覆盖配置的外部输入稳定性，不声称 MiniNDN 或完整系统
runtime 身份已验证。下一步完成实际 import/runtime 核查与 native
receipt 刷新，再重审 T007，保持正式资格门顺序。
