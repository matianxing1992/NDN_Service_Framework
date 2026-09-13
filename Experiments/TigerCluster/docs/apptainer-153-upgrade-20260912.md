# Apptainer 1.5.3 Host Upgrade

**Updated**: 2026-09-12 22:59 -05:00
**Status**: PASS (host install and minimal SIF smoke only)

## Source and Installation

按用户要求，本机Ubuntu20.04 x86_64从手工安装的 `/usr/local/bin/apptainer` 1.3.4切换到官方deb管理的 `/usr/bin/apptainer` 1.5.3。
来源：https://github.com/apptainer/apptainer/releases/tag/v1.5.3 。
`apptainer_1.5.3_amd64.deb` SHA-256：`82b0bdddf459087d202383360b8318d526ad6826c748a2f669913cc6aef9ee40`，与官方GitHub release asset digest一致。
APT新增apptainer及libprotobuf-c1，无其他包升级/移除；未安装suid附加包，旧安装也没有starter-suid。

旧binary、singularity链接、libexec、配置、补全和man页移到root专用 `/var/backups/apptainer-upgrade-20260912/`，不再参与运行。
`/usr/local/bin/apptainer` 和 `singularity` 改为指向 `/usr/bin/apptainer`，保留历史显式路径调用兼容；新配置使用 `/etc/apptainer`。
旧镜像、模型、私有身份及缓存没有删除或迁入Git。

## First Boundaries and Resolution

安装postinst报告AppArmor2.13.3无法解析包内 `abi/3.0`。包内只是unconfined placeholder，本机也无 `apparmor_restrict_unprivileged_userns` sysctl。
保留原profile于备份目录，将 `/etc/apparmor.d/apptainer` 改为说明兼容原因的注释文件，不关闭系统AppArmor。
重新执行包postinst成功且无错误；将来升级OS时应恢复匹配版本的官方profile。
一次 `dpkg --configure apptainer` 因包已配置而拒绝，未视为重新配置成功；随后直接验证postinst。

历史 `images/spec180-runtime-r119.sif` 链接目标不存在，首次exec在镜像路径检查即失败，不是运行时或NDNSF协议失败。
改用独立rootfs（host /bin/true及其两项动态库）构建最小SIF，不下载/重建NDNSF镜像。

## Validation

- `dpkg-query`：`apptainer 1.5.3 install ok installed`。
- 默认 `apptainer version`、`singularity version`、`sudo -n apptainer version` 均1.5.3。
- `apptainer build smoke.sif rootfs`：exit0；`apptainer sif list`可读取SIF。
- 普通用户及sudo的 `apptainer exec --cleanenv smoke.sif /bin/true` 均exit0；最小rootfs缺passwd/group产生提示，未影响退出码。
- `/usr/bin/apptainer` SHA-256：`2cbfdcbc53a0a1eb56a1327cc42c4cfbdb48beaa03b9a26547df9e4556d3b673`。
- smoke SIF SHA-256：`3e3660a7b4755444443414448cae25f5dbbad797afdda5bafd07db3008bcf9fb`。
- 原始安装/postinst/build日志及smoke位于 `/tmp/ndnsf-apptainer-153-upgrade/`，是本机临时证据；本记录保留精简持久结论。

## Remaining Scope

计算节点1.5.3来自用户确认，本轮未SSH或提交Slurm核验；新实验须在作业中记录实际版本。
未运行完整NDNSF SIF构建、fakeroot package安装流程、GPU或Tiger资格。下一步在实验机安装同版本，按固定源码基线重新准备候选及执行所选实验。
