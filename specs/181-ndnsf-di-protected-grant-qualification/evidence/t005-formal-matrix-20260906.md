# Formal Local Y-N Matrix

**Status**: IN_PROGRESS (fixture closure focused PASS; formal matrix pending)
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
