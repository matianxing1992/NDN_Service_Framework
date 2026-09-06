# Committed Native Build Closure

**Status**: BLOCK
**Evidence layer**: source inspection / executed (focused configure and build preflight)

## First Boundary

为区分已提交源码与预存工作区修改，从 `d67de87a` 创建 detached
检出 `spec181-candidate-checkout-20260905-r1/`，路径位于 ignored
workspace temporary directory。没有复制主工作区的 41 个 dirty
native fingerprint 输入。使用与主工作区相同的 configure 参数，
启用 tests/examples、禁用 local dependency prefix，并绑定现有
ndn-svs source/build，configure PASS（10.975s）。

维护 `spec180_native_build.py build --jobs 2` 在任何 C++ 编译前退出：
`tests/wscript:150` 引用的 `integration-tests/ndnsf-di-native-assembly.t.cpp`
未入 Git，`find_node` 返回 None，Waf 报
`AttributeError: 'NoneType' object has no attribute 'name'`。
原始配置/构建日志保留在 `spec181-candidate-audit-20260905-r1/`。
这是提交源闭包失败，不是协议或模型结果。

## Repair Boundary

补齐已被维护 Waf 清单引用的 native assembly 集成测试，按其命名
selector 编译/执行后提交；不禁用 tests 绕开构建图错误。
同时核查 native identity 是否覆盖实际加载的 Waf 控制文件。
本记录不关闭 T007；没有执行正式矩阵或 SIF 构建。

同一轮 identity 回归还确认，实际被 Waf 加载的 `tests/wscript`
不在 native source fingerprints 内。保持 mtime 不变但修改其字节后，
维护 `verify` 仍接受旧身份；R1 **1 FAIL（0.35s）**，69 deselected。
这不是测试文件本身的模型结果，而是构建控制输入遗漏。先把该
控制文件纳入身份清单，再执行维护 identity 与 assembly 定向检查。

## Focused Validation R2

已把 `tests/wscript` 纳入 source fingerprints，维护 Python identity
回归 **70 PASS（2.21s）**。集成目标构建 PASS（10m0.442s），随后仅执行
`Spec175NativeAssembly` 的 7 个命名用例：**3 PASS / 4 FAIL**，
44/48 assertions 通过。四个失败均在 Python certified assembly 的
`recipe_digest` 校验处，尚未到 ORT 装载；不能计为模型执行失败或资格结果。
R2 原始日志保留在 `spec181-candidate-audit-20260905-r2/` 的
`identity-green.log`、`integration-build.log`、`assembly-focused.log`。
下一步核对测试构造的 recipe 字节与生产契约后，在新 R3 目录复验。

## Focused Repair R3

已定位为旧 C++ fixture 的维度序列化：测试把 `1/2/8` 作为 JSON
字符串参与 recipe 摘要，而生产装配请求按契约发出整数；`sequence`
仍应为符号字符串。修正 fixture 的摘要构造，不更改生产摘要检查。
增量集成目标构建 PASS（30.613s）；同一 selector **7 cases / 160
assertions PASS**，进程退出 0。日志保留在
`spec181-candidate-audit-20260905-r3/`。

该 selector 使用小型 ONNX fixture 和受控获取回调，覆盖缓存/篡改
拒绝、共享 canonical assembly 的 ORT 装载、YOLO Merge 与 canonical
offer。1/2/4 Provider 用例为进程内模拟；没有真实多 Provider 网络、
Qwen 模型资格或 SIF 结果。源码闭包单元可以提交；随后必须从新提交
重新创建干净检出验证维护 native build，才可判断剩余提交源闭包。

文档检查最初误用不存在的 `scripts/audit_speckit_structure.py`，只产生
路径错误；维护入口为 speckit-audit skill 的 `scripts/` 下同名脚本。
这不影响编译/测试结果，不构成协议尝试。
