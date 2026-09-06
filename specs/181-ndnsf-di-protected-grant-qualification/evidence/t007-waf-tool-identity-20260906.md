# Local Waf Tool Identity

**Status**: PASS (Waf source/selection unit only; T007 BLOCK)
**Evidence layer**: implemented inspection / executed (focused fixture boundary)

## First Boundary and RED R1

`spec180_native_build.py` 绑定 Waf 启动脚本及 `.waf-tools`，未记录
实际 `waflib`。维护 `waf::find_lib` 首先使用有效 `WAFDIR`，其次
查找安装目录，最后使用本地解包目录；因此只核对启动脚本不能
证明实际构建实现保持一致。修复 owner 仍为维护 native identity
helper，不新增独立 builder，也不涉及 SIF/Tiger。

R1 **5 failed, 70 deselected in 0.36 s**：修改/新增/删除 Waf 实现
文件和切换 WAFDIR 的四项在旧 verify 中均未拒绝；解释器修改项
虽被 runtime 比较拒绝，却已越过 native import probe 边界，未在
构建工具检查阶段拒绝。测试使用真实临时文件和模拟 native/build
进程，没有编译、模型、网络或资格运行。

命令：`python3 -m pytest -q tests/python/test_spec180_native_build.py -k waf_runtime_identity_drift`。
`focused-red.log` 与 `source.patch` 保留于 ignored workspace temporary
directory 的 `spec181-waf-tool-identity-20260906-r1/`。

## Repair Contract

从维护启动脚本的静态常量解析工具目录选择，读取选中 waflib 的
实际文件与解释器身份；不 import/执行工具来计算身份。构建时将
选中的 WAFDIR 显式传给 Waf，前后核对身份，并写入 native receipt。
verify 在 native import 前核对工具身份，结束后再次核对；旧 receipt
缺少此平面时必须重新通过维护构建生成。该检查是本地回归身份，
不声称签名供应链证明或阻止校验结束之后的外部修改。

## Fixture Boundary R2

初次实现后的维护文件检查为 **57 failed / 18 passed in 2.77 s**。
首因是旧 fixture 的解释器文件没有执行权限，新的真实 PATH 解析
因此报 `WAF_PYTHON_MISSING`；另有一个旧断言未包含新的 child-only
WAFDIR。修正 fixture 执行位与环境断言后再验，不放松生产解析。
R2 `focused.log`、`source.patch` 已保留，无真实 native/build 子进程。

## CLI Fixture Boundary R3

R3 **1 failed / 79 passed in 1.81 s**。剩余旧 CLI 测试用 fixture
PATH 构建 receipt，随后用 ambient PATH 验证；新工具身份检查正确
地先报 WAF_TOOL_CHANGED，尚未到该测试原定的错误 Core 链接边界。
将该测试的实际 CLI PATH 对齐 fixture 后，再检查原链接拒绝断言。
R3 日志与补丁已保留，不修改生产检查顺序。

## Focused GREEN R4

R4 **80 passed in 1.75 s**，命令为
`python3 -m pytest -q tests/python/test_spec180_native_build.py`。
原五项回归均通过，且验证构建 child 显式 WAFDIR、父环境保持不变、
相对 PATH/WAFDIR 按构建 cwd 解析、构建/验证期间漂移拒绝并保留
上一 receipt，以及缺少 waf_tool 的旧 receipt 在 probe 前拒绝。
R4 `focused.log`、最终 `source.patch` 已保留。

同目录 `actual-selection.log` 记录实际主工作区和 `1ba99000` 隔离
checkout 的只读对照：使用维护 Waf 启动脚本的非 main 路径读取
真实 wafdir，与新 owner 均为 WAF_SELECTION_MATCH，每个选中树
记录 80 个源/资源文件。该对照不调用 Waf build 或 native import。

新 receipt 必须包含 `waf_tool`，并参与 binding reuse 判断；已有
native receipt 不能直接补字段复用，须在后续交付源码确定后通过
维护 builder 重新生成。标准 Python bytecode cache 不属于本次
源/资源文件清单；该单元不声称覆盖缓存篡改、完整系统工具链供应链
或运行进程的持续证明。外部 import/模型输入及完整 runtime closure
仍属 T007/A05 的剩余核查；本单元 PASS 不增加任务完成数。
