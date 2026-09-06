# Local Gate Source Identity

**Status**: PASS (focused committed-source validation); T007 BLOCK
**Evidence layer**: source inspection / executed (fixture children only)

## First Boundary

维护 `scripts/run_spec180_local_gate.py::run_local_gate` 校验清单字段、
条目脚本/二进制哈希和命令形状，但未把 `sourceRevision` 与实际
checkout 核对。`spec180_inventory.py` 仅检查该字段的字符串格式；
因此条目哈希正确并不能证明整个运行源码属于声明的提交。

使用现有 `test_spec180_local_gate.py` 的 fixture 构造器和真实 gate
执行函数复现：`/tmp/spec181-gate-identity-0fj11yan` 不存在 Git HEAD
（查询退出 128），清单填入 40 个 `b` 的虚构 sourceRevision，gate
仍返回 **PASS**。六个 fixture 子进程均退出 0、cleanup PASS。
这些子进程只执行小型脚本/测试，没有 MiniNDN、模型或正式资格运行。

原始结构化记录保留在 ignored workspace temporary directory 的
`spec181-local-gate-identity-20260906-r1/probe.json`；fixture 与输出目录
保留在上述临时路径。该 PASS 是需要修复的 gate 裁决，不是资格证据。

## Repair Boundary

下一单元为 T008 的正式 local gate 加入实际 checkout/提交与未提交
源码校验，并证明错误身份在任何子进程和结果目录创建前被拒绝。
既有缺 oracle、崩溃和清理 fixture 应使用明确封存的对应源码，不能
绕开新门。`effectiveConfigDigest` 当前亦为传入声明，其实际消费
绑定语义仍须核查；本记录不声称已确定或关闭整个配置封印设计。

T007 仍 BLOCK，整体保持 5/12；不先运行正式资格矩阵发现这些基本
身份缺口。CodeGraph 的旧调用图曾把无实际 import 的生成器关联到
候选 helper，已用当前源中的精确 import 搜索排除，不据此引入依赖。

## Focused RED R2

现有 gate fixture 改为显式提交的小型 Git 仓库，新增九种真实源码
变异：无 Git、错提交、dirty 后缀、未暂存/暂存修改、assume-unchanged
隐藏修改、未跟踪输入、被 ignore 隐藏的代码、执行位变化。
九项全部失败（0.63 s），均到达被测试断言替换的 `_run_entry`；
尚未启动资格子进程。失败证明原 gate 缺少相应拒绝，而非 fixture
初始化失败。既有缺 oracle fixture 同步封存变更后的源码身份。

原始记录：ignored `spec181-local-gate-identity-20260906-r2/` 下
`focused-red.log` 与 `source.patch`。下一步在条目哈希验证后、结果
目录创建前加入实际 checkout 检查；保留生成产物与源码的边界，
配置/依赖/候选的完整绑定仍由 A05 后续审查关闭。T007 BLOCK，5/12。

## Focused GREEN R3

维护 gate 在所有条目命令/哈希通过后、创建结果目录前校验实际 Git
根和 HEAD、index 与 commit 树，以及 tracked 文件原始 Git blob 字节
和执行位。校验不依赖 Git 的 stat 缓存或 assume-unchanged 标记。
未跟踪输入及 ignore 隐藏的代码拒绝；生成构建产物保留独立身份平面。
可选 submodule 缺席/空目录不提供源码，已填充时递归核对固定提交。

`test_spec180_local_gate.py` 与 `test_spec180_inventory.py` 共
**23 passed in 2.39 s**，exit 0；R2 的九项拒绝全部通过，既有真实
fixture 子进程监督、缺 oracle/redaction 和条目哈希回归仍通过。
原始 R3 `focused.log`、`source.patch` 已保留。下一步核对真实构建
checkout 的生成产物边界并补根/子模块回归；配置与候选闭包仍未关闭。

## Configured Checkout R4

对实际 `1ba99000` 隔离 checkout 仅调用源码校验（未启动资格子进程），
首次拒绝 `RELEASE/NDNSF-UAV-nixos-aarch64.closure.gz`。检查确认 commit
存储 134-byte Git LFS pointer，而 checkout 是其声明的 240376592-byte
已物化工件；`.gitattributes` 也声明 LFS。这是新校验未处理仓库既有
LFS 编码的实现缺口，不能归为该构建源码被篡改或协议失败。

R4 `checkout.log` 保留；下一步按已提交 pointer 的 SHA-256 与 size
直接校验物化字节，不执行 clean/smudge filter、不下载工件，也不
豁免整个 RELEASE 目录。新增匹配/变异回归后再验证真实 checkout。

## Focused R5 and Generated Waf Boundary

LFS 物化字节/size 变异、错误子目录根、ambient Git 重定向、生成
build 配置、已填充 submodule 的提交/字节变异均已补覆盖；两文件
**33 passed in 2.51 s**。真实 checkout 通过 LFS 边界后，下一处拒绝
为 `.waf3-2.0.24-c88b74123ce8b9d1a27999f7cf96dff0/waflib/Build.py`。
这是 tracked `waf` 的生成工具目录，非未提交的项目实现；R5 两份
日志保留。将严格匹配 Waf 生成目录布局的代码归入构建工具身份平面；
其实际字节绑定仍须在 A05 配置/工具链闭包审查，不能由目录名证明。
不得把任意 `.waf*` 目录或普通 ignored 代码一并放行。

## Focused R6 Compatibility Failure

新增符号链接边界及运行结束后的源码复核，保留全部子进程证据并在
运行期间源码改变时返回 UNQUALIFIED。R6 为 **23 failed / 16 passed
in 4.28 s**：当前 Python 不提供 `Path.is_relative_to`，失败均先于
所需源码边界。随后的只读 checkout probe 也遇到同一 AttributeError；
没有资格子进程。两份日志保留，下一步改用已有 `relative_to` 的
ValueError 分支以兼容维护解释器；先确认 focused 退出 0 再运行 probe。

## Focused GREEN R7

维护 Python 兼容性已修正，两个文件 **39 passed in 3.44 s**，exit 0。
包含 R2 九种身份拒绝、LFS 物化 size/hash、根目录/Git 环境隔离、
submodule、符号链接越界/未封印目标、生成 Waf 布局分类和执行期间
源码变化。最后一项运行真实 fixture 子进程：各子项均 PASS 且清理
完整，但最终源码变化使 aggregate 为 UNQUALIFIED，并保留
`sourceIdentity.status=FAIL` 与具体 reason。

随后仅对实际隔离 `1ba99000cd6b03705ea96f0b176d6a77fbbe8a1d` 执行
源码检查，exit 0，`SPEC181_SOURCE_CHECKOUT_OK`；没有运行模型或
资格子进程。R7 的 `focused.log`、`checkout.log` 和 `source.patch`
保存精确结果与修复字节。LFS 原始字节检查无需下载或执行 Git filter。

本单元关闭 gate 的提交/源码身份校验，未关闭配置/工具链平面：
`effectiveConfigDigest` 的实际消费、生成 Waf/build 字节、运行时与
外部 Python import 路径仍须绑定。T007 保持 BLOCK；既有模板字段
通过不证明这些配置真实一致，下一步按本地验证交付范围继续核查。

## Checkpoint Gate R8

本地 commit hook 拒绝生产脚本中列出的 assistant 临时目录例外，
未生成提交。删除这些非产品特例：候选若含相应额外代码，应接受正常
ignored-code 拒绝，不能因工具目录名获得豁免。不修改或绕过 hook。
下一次定向/真实 checkout 核对使用 R8 新目录，原有 R7 证据保留。
R8 重验 **39 passed in 4.46 s**，实际 `1ba99000` checkout 也返回
`SPEC181_SOURCE_CHECKOUT_OK`、exit 0。两份新日志与最终 source patch
保留；删除特例后没有放宽其他源码拒绝边界。
