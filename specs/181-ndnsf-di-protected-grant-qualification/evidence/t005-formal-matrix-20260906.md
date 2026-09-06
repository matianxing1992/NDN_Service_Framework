# Formal Local Y-N Matrix

**Status**: IN_PROGRESS (sudo source repair PASS; formal retry pending)
**Evidence layer**: implemented / executed (source regression; no network yet)

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
