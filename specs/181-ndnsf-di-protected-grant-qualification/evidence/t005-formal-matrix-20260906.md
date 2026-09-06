# Formal Local Y-N Matrix

**Status**: BLOCK (backend focused repair PASS; native identity refresh/re-audit pending)
**Evidence layer**: implemented / executed (formal network startup and focused regression)

## Subject and Launch R1

T007 convergence 已在 `c3a9d7df` 提交 PASS。隔离检出更新至该提交，
沿用已经验证的同一 native/Repo 构建；只有审计/进度文档发生变化。
正式入口为维护 `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --case Y-N`，
按 Y-N-O/C/P/R/I/E/L 顺序执行；Y-N-E 内部三变异全部要求实际拒绝。

原始目录：ignored workspace temporary directory 下
`spec181-t005-formal-20260906-r1/`。`launch.py` 只记录明确环境、输入
摘要、source guard 与维护 CLI 的退出，不生成或替代矩阵 verdict，
不重试。命令：`sudo -n /usr/bin/python3 <R1>/launch.py`，stdout/stderr
保存到 R1 `run.log`。独立 case/state/PIB/TPM；启动前无运行中 NFD。
输入沿用已预检模型/信任映射，显式 config root 为现有受保护配置。
不在日志或 Git 保存私钥内容。

首次失败停止并保留原始子结果；只有七子用例全部达标、清理退出
完整、源码及输入摘要不变，才能关闭 T005。SIF/Tiger 不在本次执行。

## First Boundary R1

R1 exit 1，清空 sudo 环境后 Git 丢失 `SUDO_UID`，把用户拥有的
检出判为 dubious ownership。失败在 launcher 的 git rev-parse，
尚未执行维护 runner 或创建网络。run.log 与新 case/state 保留。
R2 将原 sudo 身份明确保留到子环境，让 Git 按真实调用用户核对
该检出；不写 global safe.directory，也不放宽生产 source guard。

## First Boundary R2

R2 外层 Git 已通过，但 `_source_git` 的独立清理环境再次移除
SUDO_UID，实际生产 source guard 报 SOURCE_CHECKOUT_UNAVAILABLE。
仍未执行维护 runner/联网。该门必须支持本机 sudo MiniNDN 的
正常用户检出；仅当 euid=0 且 SUDO_UID 等于检出实际 owner 才保留
这一标准 Git 身份，仍移除 GIT_DIR/index/config/replacement 等输入。
先添加真实 sudo/用户检出回归再修复，A05 受影响项暂时重开。

## Sudo Source Regression R1

`spec181-sudo-source-20260906-r1/red.log`：真实 sudo + 用户拥有的
临时提交检出，**1 failed / 1 passed**。合法 owner 无法通过，错误
UID 按预期拒绝；同时注入 GIT_DIR/GIT_INDEX_FILE 验证它们不成为
源码权威。下一步只保留匹配 owner 的 sudo UID，再跑这两个真实
用例与既有 local gate 回归。

## Sudo Source Repair R2

维护 `_source_git` 仅在 root 且 SUDO_UID 与实际检出 owner 相等时
保留该字段；Git 配置/索引/替换对象覆盖仍全部清除。真实 sudo
正负例与既有 gate 回归最终 **51 passed（8.68 s）**，见
`spec181-sudo-source-20260906-r2/final.log`；之前 51 passed（8.60 s）
也保留。owner 读取放在原错误处理内，缺失检出仍受统一拒绝处理。
此修复不修改 native/应用行为；A05 受影响项复审 PASS，T007 恢复
PASS。下一步提交修复，用新 R3 执行维护正式矩阵。

## First Boundary R3

修复已提交 `88e7a458`。R3 前置源码核对通过并进入维护 CLI，
exit 78：`REQUEST_ENVELOPE_KEY_OWNER_MISMATCH`。原 envelope key
由开发用户拥有，root MiniNDN 的现有输入契约要求当前执行 uid
拥有该文件。未创建网络/子用例；前后源码和输入保持一致。
下一步将相同 key 字节复制到新 R4 专有 state（root owner、0600），
显式引用副本；保留原 key 的所有者/权限与原始 R3，不改校验契约。

## First Boundary R4

R4 的 envelope owner 已满足；维护预检继续在
`CASE_CONFIG_ROLE_SET_INVALID:Y-N` exit 78。前次 Y-B 预检环境只
声明四个共享角色，Y-N 必须额外声明 FullModel 能力。无网络启动。
既有 `/tmp/spec181-y-n-run/env.sh` 是 Y-N 输入集，下一步核对其
五角色/四 Provider 覆盖及全部实际输入字节，再以新 R5 执行。
R4 原始结果保留；不在 runner 放宽注册角色集合。

## First Boundary R5

既有 Y-N 配置确有五角色，同一模型包；须补设保护纪元以执行
Y-N-E。R5 在临时 launcher 的变量名白名单断言处 exit 1：Y-N
环境含已注册 `SPEC180_CASE_OUTPUT_DIR`，旧 Y-B 解析器未接收它。
尚未创建 case/state 或执行维护 runner。下一步允许这一明确字段，
仍由当前 run 专有输出路径覆盖；不扩大生产环境或变更协议。

## First Boundary R6

R6 已进入维护 runtime，Mininet 的可执行文件检查找不到 ifconfig，
exit 1；显式 PATH 仅含 bin 目录，遗漏系统 sbin。没有产生协议
结果。下一步显式追加 /usr/sbin、/sbin、/usr/local/sbin，并核对
ifconfig/ip/tc/ovs-vsctl/mnexec/NFD/ndnsec/NLSR 可定位；Python 与
native 选择优先级保持原值，实际有效环境重新记录。R6 日志保留。

## First Network Boundary R7

R7 在 Y-N-O 启动五个 NFD，五个 socket 均存在，但 nfdc face list
全部未就绪，维护矩阵 exit 2。`nfd-startup-failure.json` 与每节点
NFD 日志保留；没有启动业务子进程，控制结果 UNQUALIFIED。
终止后确认无 NFD/native Provider 残留，源码/输入前后不变。

排查发现 launcher 继承了离线 preflight 的 NDN_CLIENT_TRANSPORT=
unused.sock（以及 PIB/TPM）覆盖；节点 client.conf 本来分别指向
/run/nfd/<node>.sock。公共 wait_for_nfd_sockets 调用节点 nfdc，
会受该环境覆盖影响。R8 移除这些全局覆盖，使用 launcher 专有
HOME/.ndn/client.conf 隔离父进程；节点继续用其自身 HOME。明确
保留已验证的 /home/tianxing/.local/lib/python3.8/site-packages
依赖路径，避免 HOME 改变导致依赖丢失。新运行验证这一诊断。

## First Application Boundary R8

R8 已越过 NFD readiness、路由与 keychain 初始化，首次创建
controller.log 后在业务进程启动边界退出，日志为空。总耗时约
19.2 s，无 90 s readiness 等待；矩阵只留下 CONTROL_NOT_PROVEN，
原始启动异常被包装/顶层处理后不可见。NFD 已全部退出。
下一步修复这一实际诊断缺口：启动异常记录类型与 traceback
文件/函数/行号，不记录异常文本、locals 或 secret；保留现有失败
裁决与清理。定向测试后在新 run 定位底层启动错误。

## Spawn Diagnostic Regression R1

`spec181-spawn-diagnostics-20260906-r1/red.log` 复现 **1 failed /
87 deselected（0.90 s）**：现有部分启动回滚仍清理，但没有任何
process-start-failure.json 可定位原异常。新增断言要求异常类型、
末端函数/行号且不出现异常文本或 locals；下一步补这个持久记录。

## Spawn Diagnostic Regression R2

实现后两文件回归 **1 failed / 92 passed（1.55 s）**；唯一失败是
新增测试未导入 json，运行时已生成预期诊断。R2 日志保留；下一步
补测试导入后重验，不能把 fixture 错误归为运行时失败。

## Spawn Diagnostic Repair R3

两文件最终 **93 passed（2.25 s）**，见 R3 focused.log。部分启动仍
回滚/关闭原子 phase；新证据记录 OSError 及真实 frame 位置，不含
异常文本或 locals，使用独占文件创建保留首次证据。写入失败不
替代原始运行异常。没有改矩阵 PASS/FAIL、重试或网络流程。
本单元受影响审计 PASS；下一步提交后新 run 定位业务 spawn 失败。

## First Application Boundary R9

诊断修复提交 `18623480` 的 R9 捕获实际 KeyError：
`mininet/node.py:419` 的 popen(shell=True) 读取 `os.environ['SHELL']`，
临时显式启动环境遗漏该字段。调用链从 legacy.start → getPopen →
Mininet.popen 已保存于 process-start-failure.json；不再把问题猜作
Controller/NFD 协议失败。R9 清理后无 NFD，源码/输入均不变。
下一步在新 R10 显式设置 SHELL=/bin/bash，不修改 Mininet 或协议。

## Controller Publication Boundary R10

源码 `1862348077426901de0e7856507b88d28714fc85`，原始目录
ignored workspace temporary directory 下 `spec181-t005-formal-20260906-r10/`。补齐 SHELL 后 NFD、
路由、keychain 与 Controller 进程启动通过；controller.log 记录
`_publish_spec180_runtime` 创建 NativeServiceUser 时抛出
`RuntimeError: Failed to acquire file lock`。维护 CLI exit 2，Y-N-O
为 CONTROL_NOT_PROVEN，尚无协议结果。launch-result.json 确认
sourceIdentityUnchanged/inputIdentityUnchanged 均 true；无 NFD 残留。
下一步定位同进程 Controller 与 publication User 的锁/存储所有权，
先定向复现修复，再恢复正式矩阵。T005 未通过。

定向 syscall 复现修正诊断：`/tmp/ndnsf-svs-registration-0.lock`
inode 2638760、owner 1000、mode 0664；root 的原始
`open(O_CREAT|O_RDWR, 0666)` 返回 errno 13/EACCES，尚未执行 flock。
`/proc/locks` 无对应 inode，sudo fuser exit 1 且无占用进程。
这是历史残留文件属主错误，不是 Controller/User 互锁。下一步将
原文件完整移入 R10 原始目录保留，随后由既有运行时创建 root 锁；
不改变全局内核保护设置，不修改 Core 锁机制。
诊断读取 `/proc/sys/fs/protected_regular` 被拒绝，未据此推断配置值。

## Fixed Input Source Closure R11

原始目录为 ignored workspace temporary directory 下 `spec181-t005-formal-20260906-r11/`，同源
`18623480`。保留旧锁后运行时创建 owner 0 的新锁；Controller 完成
APP 发布，Repo 与四个 native Provider 就绪。User 读取固定输入时
FileNotFoundError：隔离提交缺少
`tests/fixtures/spec180/yolo26n/fixed-fixture.ppm`。主工作区存在该
未跟踪文件及 README，162 字节固定 PPM 的 SHA-256 与候选 manifest
完全匹配。source/input 前后不变、CLI exit 2、Y-N-O 未通过，已清理。
这是提交源码的数据依赖遗漏；A05/T007 对此边界重新 BLOCK，下一步
将既有固定输入及说明纳入交付，定向验证实际 load_reference 和数值
回归后重新审计。不能向隔离检出临时注入未跟踪 fixture 冒充同源。

## Fixture Closure Repair

原始目录为 ignored workspace temporary directory 下 `spec181-fixture-closure-20260906-r1/`。
既有 `test_spec180_yolo_numerical.py` **22 passed（7.97 s）**，包括
真实 User 输入编码、固定预处理、数值/摘要拒绝与结果记录；该测试
本轮未修改。实际 canonical package 的生产 `load_reference` 通过，
固定输入摘要为 `sha256:7edf1f524ef450be6ee2304b3c0b47b70c3d72610c18f2f2fa28157d7b8a113c`，
输入 shape `[1,3,640,640]`、oracle shape `[1,50,6]`。将原有 PPM
及其 README 原字节纳入提交，未替换 oracle 或模型。A05 受影响项
复审 PASS；下一步在新提交的隔离检出重复实际 loader 检查后运行
新矩阵。本记录仍不构成 T005 PASS。

## Subcase Epoch Boundary R12

隔离提交 `344fb52fe7b264ea114ae2cb60dab57d748248ae` 的真实
load_reference 已通过（fixture repair 的 committed-reference.log）。
正式原始目录为 ignored workspace temporary directory 下
`spec181-t005-formal-20260906-r12/`。User 越过输入加载后，在
_build_grant_seam 报 SPEC181_REQUESTER_PRIVATE_KEY KeyError。
生产 _run_live_case_once 仅对 Y-B/Y-N-E 配置 protected runtime
publication 和 grant keys，却让其余子用例继承 matrix 顶层的保护
epoch；Y-N-O 因而错误进入 grant seam。矩阵 exit 2，没有控制 PASS。
这属于 runner 子用例配置不一致；暂停矩阵，按子用例统一 child epoch，
以定向测试证明 control 与三种 Y-N-E 仍分别走预期语义后复审。

## Subcase Epoch Regression R1

`spec181-subcase-epoch-20260906-r1/red.log` 记录 **7 failed / 4 passed /
88 deselected（2.06 s）**：Y-A 及六个明文 Y-N 子用例全部复现 child
epoch 与 publication 输入不一致；Y-B 与三种 Y-N-E 已保留 protected
epoch。测试在真实 _run_live_case_once 的 native preflight 边界捕获
环境，不启动网络。下一步将 child epoch 绑定该函数选定的 runtime
输入，并核对父环境保持不变，不能修改 matrix 顶层的保护要求。

## Subcase Epoch Repair R2

`spec181-subcase-epoch-20260906-r2/focused.log`：**117 passed（3.99 s）**，
覆盖 runner、完整矩阵调度/失败保留和 grant seam。新 11 项实际 runner
环境捕获验证 child epoch 与 publication 一致：Y-A/O/C/P/R/I/L
为 plaintext-v1；Y-B 及 Y-N-E 的三变异保留 protected epoch 与
requester/recipient 配置；父进程环境保持不变。仅修复环境选择，
未改变受保护授权验证、协议 oracle 或自动重试策略。T007 受影响项
复审 PASS。R12 source/input 前后仍一致，且所有 NFD 已退出。
下一步以新提交执行新正式矩阵；T005 继续未完成。

## Canonical Binding Boundary R13

源码 `9dd5c62c3883001206f601dbaddf1b08dc751907`；原始目录为
ignored workspace temporary directory 下 `spec181-t005-formal-20260906-r13/`。
User 越过 epoch seam 后发起 V3 request，YOLO ensurer.describe 给
CanonicalArtifactBinding 传 canonical_graph_digest 时 TypeError。
主工作区 canonical_artifacts.py 的共享字段/引用扩展未进入提交，
而已提交的 YOLO adapter 与 placement 消费这些字段。CLI exit 2，
Y-N-O CONTROL_NOT_PROVEN；源码/输入不变，全部 NFD 已退出。
暂停矩阵、重开 A05，审查绑定与后续 publication 所需依赖，在隔离
源码先定向验证完整 describe/recipe/publication 路径后再复审。

## Canonical Binding Regression R1

`spec181-binding-closure-20260906-r1/probe.py` 使用同一签名 canonical
package，在隔离检出直接调用真实 adapter/binding；red.log 首先在
SDK import 因缺少 Repo Python 路径而失败，尚未复现构造错误。
下一次补全已验证的显式 Python 路径与私有离线 NDN 环境。
审查现有 canonical_artifacts.py 变更均为同一 source/initializer
引用契约与绑定扩展；artifact_deployment.py 及既有 canonical tests
提供其公共发布/消费 owner 与回归，一起形成可测试的完整依赖单元。
下一步将该单元映射到隔离源码，验证实际 YOLO 两候选发布及旧调用兼容。

## Canonical Binding Regression R2

补齐 Repo/native Python 路径及私有离线 PIB/TPM/transport 后，
`spec181-binding-closure-20260906-r2/red.log` 在真实
YoloCanonicalArtifactBinding.describe 精确复现 R13 的 TypeError。
未启动网络。现在执行已审查的共享依赖单元投影和定向回归。

## Canonical Binding Repair R3 and Recipe Boundary R4

R3 共享依赖投影的 24 项 canonical/candidate 回归 PASS（1.16 s）；
真实签名 YOLO 两候选的 describe/三对象 publication probe PASS。
R4 增加真实 _v3_role_specs → _certify_v3_role_specs 后，发现提交的
CertifiedOnnxAssemblyRecipe 仍拒绝 COMPONENT_SET 的零 layer interval，
与已提交 placement 的 node-set 语义不一致。R4 保留独立 probe.py
及失败日志；R1 probe.py 保持原版。继续 BLOCK，不运行网络。
现有 executor 修改中，component-set、external initializer 与 shape
归一化属于当前 canonical assembly 单元；CUDA provider-chain 为
未纳入的预存实验改动。仅投影前者并跑实际 recipe 与 assembly parity。

## Canonical Assembly Boundary R5

R5 的 Python/reference/candidate 检查 **32 passed**；8 项 native
parity 因未指定 SPEC181_ASSEMBLY_PARITY_BINARY 失败，当前隔离
构建未生成该目标，不能把配置缺失记为 parity 拒绝。recipe probe
已越过原构造错误，但断言把无 ONNX 装配的 Merge 也计入 canonical
graph 检查；生产明确跳过该角色。下一步改 probe 的角色断言并构建
实际 native parity 目标，保留 R5 的 8 failed / 32 passed（1.80 s）。

## Canonical Binding and Assembly Repair R6

原始目录 `spec181-binding-closure-20260906-r6/`。隔离源码使用
既有 Waf `spec181-assembly-parity` 目标，编译/链接 PASS（60.684 s）；
binary SHA-256 为 `9a3d1842d1407b614ad4c8167d6c4b0b9c921dc85d6ad307de0a68192cd93543`。
设置 SPEC181_ASSEMBLY_PARITY_BINARY 后，canonical/candidate 与
固定装配检查 **40 passed（12.91 s）**，包括 8 个 Python 和 8 个
实际 C++ 入口向量；四种拒绝与正例模型字节/ORT CPU 输出均通过。
实际签名 YOLO 两候选的 describe → role specs → certification →
publication port probe 通过；三对象按 graph/initializer/root 发布，
引用摘要与 source/initializer 绑定，Merge 保留独立 native 语义。
publication 使用内存测试 port，不声称此 probe 证明网络或加密往返。

提交单元为 canonical_artifacts.py、artifact_deployment.py、
executor.py 的 component/external-initializer/shape 改动，以及
canonical tests。其余预存改动（包括 CUDA provider-chain）留在工作区。
隔离测试字节与选定提交字节逐文件核对；A05 受影响项复审 PASS，
下一步以新提交恢复正式矩阵。T005 仍未完成。

## Sealed Plan Boundary R14

源码 `9da4d92c362a52c7fbd9d816a351f0c3239f3729`；原始目录
`spec181-t005-formal-20260906-r14/`。User 进入 SealedCollaborationPlan
构造时 TypeError：artifact_fetch_data_names 字段仍未纳入提交，
但已提交 placement 会传递该映射。现有 plan.py 修改区分 canonical
artifact identity 与 fetch transport reference，并纳入 plan_digest。
CLI exit 2，Y-N-O CONTROL_NOT_PROVEN，源码/输入不变且 NFD 已清理。
暂停矩阵、重开 A05；以计划封存/消费及摘要变异检查闭合此公共契约。

## Sealed Plan Regression R1

`spec181-plan-closure-20260906-r1/red.log`：**10 failed（1.72 s）**，
均在实际 V3 SealedCollaborationPlan 构造表达式复现缺字段 TypeError。
新增测试执行生产 sealing 与 commit_plan 表达式，捕获传递给 Core
port 的 canonical/fetch 区分，另检查 digest 变异、不可变性、旧调用
默认值和错误引用拒绝。先投影既有 plan 扩展，再检查其语义边界。

## Sealed Plan Validation R2

投影后 **5 passed / 5 failed（0.75 s）**：正常 transport/空引用、
旧调用回退、角色覆盖与相对路径拒绝通过；None/False/0 及含换行/
NUL 的引用被错误接受。下一步保留仅空字符串表示本地 preparation
的约定，拒绝非字符串与控制字符，再回归实际 commit port。

## Sealed Plan Repair R3

`spec181-plan-closure-20260906-r3/`：隔离源码中 22 项直接表达式/
候选回归 PASS（0.97 s），另 36 项既有 plan sealer、YOLO ACK
planning 和 automatic collaboration plan 回归 PASS（0.77 s）。
实际 V3 sealing → Core commit port 传递 fetch name，canonical
identity 保留；外部 map 修改不改变已封存 digest，transport 变更
改变 digest，非法 map 在 commit 前拒绝。保留 legacy 缺省回退和
local preparation 的空字符串。无网络副作用，不构成 Core 传输证明。
本次 plan.py 与新增测试的隔离/提交字节一致；A05 复审 PASS，
新提交后恢复正式矩阵，T005 未完成。

## Provider Response Boundary R15

源码 `fe3d687bb95e837a4014a341f29e8acba8b08b38`；原始目录
`spec181-t005-formal-20260906-r15/`。四 Provider 接受 preparation，
User 成功封存计划并发出 SELECTION_COMMITTED，随后终端
REMOTE_RESPONSE_FAILED，矩阵 exit 2、Y-N-O CONTROL_NOT_PROVEN。
WARN 日志只有 duplicate-request-and-token 拒绝，尚无 native
执行错误细节；不能据此断言重复请求就是根因。源码/输入前后
不变，所有 NFD 已退出。下一步检查 Core 请求/Selection 处理边界，
补有界诊断日志后定位首次实际拒绝，保留全部失败。

## External Assignment Boundary R16

同源 `fe3d687b` 的新原始目录 `spec181-t005-formal-20260906-r16/`，
仅将 Core ServiceProvider/ServiceUser 的 NDN_LOG 提升至 INFO，
其余 case 行为不变。四 Provider 依次 selection received → handler
queued → failed，首次失败信息一致：external collaboration assignment
size or digest mismatch。并非模型执行，也不是 duplicate request
日志造成的首次拒绝。矩阵 exit 2；下一步定位真实 assignment
publication/fetch/validation 的字节契约，先定向修复再恢复矩阵。

## Provider Digest Probe R1

原始目录 `spec181-provider-digest-20260906-r1/`，实际 Core 两端 helper
的编译探针在链接阶段失败：系统 g++ 由 PATH 选中 Homebrew ld，
Boost 间接依赖未解析。结果为 4 setup errors（1.60 s），尚未比较
摘要，不能记为协议 RED。下一次指定 `-B/usr/bin`，使用独立 R2
记录；同时为大 payload 设置短测试 ID，避免失败报告展开全部字节。

## Provider Digest Regression R2

`spec181-provider-digest-20260906-r2/red.log`：4 failed（1.65 s）。
系统工具链成功编译从实际 ServiceUser/ServiceProvider 源码提取的
helper；空字节、ASCII、二进制及 30 KiB 外部 assignment 均验证
User 与独立 hashlib SHA-256 一致，Provider 仅十六进制大写不符。
当前提交 Provider 未纳入工作区既有小写归一化修复；下一步仅投影
该 hunk。大小/摘要严格比较保持不变；此探针尚不证明真实网络获取。
R16 launch-result 复核 sourceIdentityUnchanged / inputIdentityUnchanged
及 sourceRevision/inputDigest/environmentDigest；前后身份一致。

## Provider Digest Repair R3

`spec181-provider-digest-20260906-r3/green.log`：4 passed（1.73 s）。
仅将既有 Provider 小写归一化 hunk 纳入隔离源码；两端实际 C++
helper 对四种输入均与独立 SHA-256 一致。ServiceUser 不变，
prepareCollaborationAssignment 的 plaintextSize 和 digest 精确比较
均保留；同一 helper 的其他消费者也获得既定 canonical 形式。
此测试编译实际函数体，不是全框架链接/网络校验。下一步本地提交、
维护 native build 重建并复审，随后新正式矩阵验证真实 assignment
边界。T005 未完成，本机 6/10 保持不变。

## Provider Digest Native Rebuild R1

修复提交 `e6f44b65`，原始目录 `spec181-provider-digest-native-20260906-r1/`。
隔离源码无 tracked/index 改动，仅允许的 build 输出目录；维护命令
`scripts/spec180_native_build.py build --jobs 2` 成功，Core/Provider
编译链接 4m8.325s，随后重新编译 Python 绑定（binding_reused=false）。
同一显式系统工具链环境下独立 `verify` exit 0，两次均输出
SPEC180_NATIVE_IDENTITY_OK；实际 DSO/SVS/provider 依赖映射通过。

| Artifact | SHA-256 |
| --- | --- |
| Native receipt | `96d972c36db284d709272ce9125175c65bbcff9cabaaf56d6a0d46a1d9d4dce4` |
| Core library | `e2d9d100d13f40e57de395c39b99b2365a18d784dc69a68bb0d405297cf52a4b` |
| Native Provider | `9fea3b07792ef77092d0823aec8034a9e6a5de65c18888237b7cbb7bd0e42705` |
| Python extension | `e9932073205b66b79e12c6f3944342d65d91fdb340b3225eb4b3326f480e1339` |

源码复核：ServiceUser 与 Provider 规范形式一致，后者只新增小写
归一化；prepareCollaborationAssignment 原有 size/digest 比较与
失败拒绝不变。4 个实际 C++ helper 正例对独立 SHA-256 通过；
该 focused RED/GREEN 与完整重建闭合此 A05 边界，T007 受影响项
复审 PASS。没有弱化 grant 或校验，也未纳入其他预存 Provider
改动。下一步 R17 验证真实获取后 assignment 处理，不能以当前
环境/构建 PASS 代替矩阵或本地资格。

## Native Backend Boundary R17

源码 `359f57e1e0f652a82c85572917f747fc22c40f55`，原始目录
`spec181-t005-formal-20260906-r17/`。四 Provider 均越过 external
assignment 大小/摘要校验并进入 collaboration handler running，
真实传输证实摘要修复有效。BackboneNeck 首先失败：
`no NativeModelRunner backend registered: onnxruntime-cpu`；其余
角色等待该上游输出后超时，是后续症状。矩阵 exit 2，Y-N-O
CONTROL_NOT_PROVEN；source/input 前后不变，NFD 全部退出。
已提交 registerOnnxRuntimeBackend 仅注册 onnxruntime，实际
Selection 使用既定公共 backend 名 onnxruntime-cpu。主工作区已有
相关注册修改，先检查其 CPU/设备选择语义和依赖，再限定修复单元。
重开受影响 A05，不降低验收或将依赖超时误记为预期负例。

## Backend Registration Probe R1

`spec181-backend-registration-20260906-r1/red.log`：新增测试多写
一个右括号，collection error（0.36 s），未启动 native 进程。
修正语法后新 R2 使用真实 di-native-provider --check-only 与小型
ONNX 模型，区分注册缺失、CPU 实际加载/warmup、非法设备请求
和未知 backend；不使用 wiring-only 或模拟 runner。

## Backend Registration Probe R2

`spec181-backend-registration-20260906-r2/`：4 failed / 1 passed
（1.02 s）。三项明确在实际 factory 缺少 cpu/cuda 公共名称处失败。
legacy onnxruntime 已真实加载/warmup、CHECK_OK，但测试将既有
字符串布尔证据当作 JSON bool，并误用顶层 deviceKind。修正测试
为真实 schema 的字符串 true 和 device.kind；不修改生产证据。
unknown-backend 的严格拒绝通过。原始每进程 stdout/stderr 保留。

## Backend Registration Regression R3

`spec181-backend-registration-20260906-r3/`：修正 schema 断言后，
legacy CPU 实际加载/warmup 与未知 backend 拒绝均通过；三个公共
名称检查仅在缺少注册处失败。接下来只投影 enabled/disabled 两个
registerOnnxRuntimeBackend 函数的既有名称注册修改。backend 名
与 metadata 中的 executionProvider 分工保持不变；当前设备选择
函数在主工作区和隔离提交中相同，不纳入其他 ONNX/KV/生成改动。

## Backend Registration Repair R4

`spec181-backend-registration-20260906-r4/`：维护 Waf 的
di-native-provider 目标编译/链接 PASS（35.522 s），五项真实
可执行程序检查 **5 passed（1.02 s）**。此前 R3 为 3 failed /
2 passed（0.91 s），首次语义失败均是缺少公共名称注册。
legacy onnxruntime 与 onnxruntime-cpu 均以真实小型 Add ONNX
模型完成 load/warmup，输出 realCompute=true、runnerKind=
onnxruntime-cpu、device.kind=cpu；没有 wiring-only 替身。
未知 backend 仍拒绝；cpu/cuda 公共名称面对 invalid-provider
metadata 均进入既有 provider validation 并拒绝，未启动 GPU，
不声称 CUDA 可执行或资格。测试不证明完整数值/网络矩阵。

只纳入 enabled/disabled 两个注册函数（16 added / 10 removed）
与真实 CLI 回归，设备选择和其他预存 ONNX/生成改动保留原状。
下一步提交该单元，刷新维护 native identity，再复审与新矩阵。
