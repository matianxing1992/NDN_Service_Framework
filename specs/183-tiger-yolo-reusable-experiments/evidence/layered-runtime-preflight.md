# Layered Runtime Compatibility — 2026-09-08

## Current Observations

在完成209983真实GPU参考之后，继续正式NDNSF-DI外置应用迁移。
Raw: `Experiments/TigerCluster/results/yolo-layered-20260908/preflight/`。

| 检查 | 实际结果 | 动作 |
| --- | --- | --- |
| 当前外置DI Python包 + 历史SIF | APP_IMPORT_PASS，Python3.10.18、ORT1.20.0 | 复用明确的/app只读部署方式 |
| 当前User入口 | 保留/app/repo/examples/python/...层级后help通过；浅路径失败保留 | 不改应用算法，只固定打包布局及所需helper |
| 已有host Provider对SIF执行ldd -r | 缺NAC Consumer::clearCache、Producer::refreshPublicParameters、Consumer::refreshDecryptionKey；要求ORT VERS_1.26.0而SIF是1.20 | 不注入host.so；更新受影响base库/消费者，app按SIF SDK重新编译 |
| 本机Apptainer | /opt/apptainer/1.5.3/bin/apptainer实际1.5.3已存在 | 复用，不重新安装；/usr/local/bin的1.3.4仅为先前本地诊断 |
| 本地原始缓存SIF | 本次独立sha256=b6710fd696a7f962f67f67a54278d92a15babb856c4af428a3f04ba54dc83285 | 当前读取匹配Tiger，旧6a3d读取失败保持历史；不再误报当前不匹配 |
| recovery-20260907完整SIF | 同大小3525861376、同b6710fd6摘要，独立inode/唯一link | 用已验证相同字节的硬链接去重缓存，释放约3.28GiB，不重下载 |

两个SIF摘要结果分别保存在old-cache-sif-sha256.txt和recovery-sif-sha256.txt。
去重仅替换recovery目录内重复缓存的目录项，原始SIF与两个plane的硬链接保留。
不改旧manifest、源码或运行记录；完整分层组合仍未资格化。
