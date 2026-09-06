# Local Launch Configuration Identity

**Status**: PASS (launch configuration unit only; T007 BLOCK)
**Evidence layer**: implemented inspection / executed (focused fixture boundary)

## First Boundary and RED R1

源码身份修复后，维护 gate 仍把调用方提供的 `effectiveConfigDigest`
原样写入结果，未核对实际传给 `Popen` 的环境与解释器文件身份。
现有源码映射确认 `_run_entry` 使用显式环境，不继承 ambient 环境；
因此问题是声明与实际输入缺少绑定，而非暗中继承全部环境变量。

R1 六项定向回归分别改变 LD_LIBRARY_PATH、PYTHONPATH、PATH、
监督器保留的输出变量、声明摘要、解释器字节。**6 failed in 0.78 s**，
全部到达测试阻止的资格 child 边界，没有执行资格子进程；原始
`focused-red.log` 与 `source.patch` 保留在 ignored workspace temporary
directory 的 `spec181-local-config-identity-20260906-r1/`。

## Repair Contract

`effectiveConfigDigest` 定义为 gate 启动配置记录的规范摘要，内容为
工作目录、完整显式环境的摘要、解释器路径/解析路径/文件 SHA-256、
CPU backend、超时、cleanup policy 与监督器输出布局。库存生成器和
gate 共用同一个计算 owner；gate 在任何输出/资格 child 之前核对，
结束后再次核对可变解释器身份。传入环境先复制，结果只记录环境
摘要，不把环境值或秘密写入证据。case 输出变量只能由监督器生成。

这不是所有运行时/模型输入的总摘要：命令和入口字节仍由 inventory
绑定；native 依赖/扩展/生成工具使用维护的 build/runtime identity，
外部模型、注册表与键配置引用由对应输入检查绑定。这些平面的实际
完整性仍待 A05 收口，不能把本单元 PASS 当作 T007 或正式资格 PASS。

## Focused GREEN R2

`local_launch_configuration` 成为 builder/gate 共用 owner；builder 从
显式环境计算摘要，旧摘要参数只作可选期望值，gate 在创建输出前
复核。环境先复制，结果记录非秘密启动配置及各 child 环境摘要；
执行后解释器变化使 aggregate UNQUALIFIED，保留子项/清理结果。

三个维护测试文件 **52 passed in 4.17 s**，包含 R1 六个拒绝与
既有源码身份/案例范围回归；没有正式资格运行。R2 `focused.log`
保留。追加真实 fixture 子进程环境消费、调用方 map 变化隔离及
解释器运行后变化的定向验证，补齐本单元的实际消费边界。

## Focused GREEN R3 and R4

R3 **61 passed in 5.34 s**。真实 fixture child 验证显式环境消费、
ambient 值不继承，以及调用方修改原 map 不改变已封存环境；末项
执行后替换解释器指向，六个 child/cleanup 虽均 PASS，aggregate
仍为 UNQUALIFIED，并保留配置身份失败原因和全部子项记录。

R4 **62 passed in 5.80 s**。新增 builder/gate CLI 联通检查：真实
pytest collection 和六个轻量 child 使用同一环境文件完成 PASS；
随后修改环境文件，gate 在输出目录/child 创建前以 exit 78 拒绝。
结果与 CLI 输出均不包含测试环境值。测试仅验证 fixture 的维护
工具边界，不是模型推理、MiniNDN 或完整 local-suite 验收。

验证命令：`python3 -m pytest -q tests/python/test_spec180_local_gate.py tests/python/test_spec180_inventory.py tests/python/test_spec181_inventory_scope.py`。
R3/R4 的 `focused.log` 分别保留于对应编号的 raw run 目录；R4
另存本单元最终 `source.patch`。R4 日志已终止且无对应 pytest 进程。

本单元关闭启动配置声明与实际消费之间的缺口；T007/A05 继续 BLOCK
于实际 native/import 依赖、生成构建工具和外部输入字节身份。
下一步复用维护 native identity owner 核查这些输入，保持 T007 →
T005 → T008 → T009 → T012 顺序。SIF/Tiger 按修订 7 移交实验机器。
