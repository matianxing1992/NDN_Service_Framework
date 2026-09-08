# Development Disk Cleanup

用户明确要求清理磁盘。清理前根分区 177G，168G used、910M available、100%；
清理后 140G used、30G available、83%。df 单位为其显示值。

- `python3 -m pip cache purge`：151 files，2050.6 MB。
- 旧 `build`、`build-nac182`、`build-system-j2` 及 detached
  `.codex-tmp/spec181-source-closure-20260906-r1/build-system-j2`：
  只删除未跟踪、非 symlink 的 `.o` / `.o.d`，2702 files，逻辑大小 26.260 GiB。
- 删除前核对主仓库与 detached worktree 的 tracked paths、实际路径包含关系和
  活动编译进程；当前 `spec182-yolo-semantic-r1/build` 完全保留。
- 保留源码、其他会话修改、模型、密钥、日志、测试二进制、共享库和原始证据。
  旧树后续重编译会重新生成这些对象文件；未声称旧二进制可证明新源码。

完整路径/大小清单：
[manifest](../../../.codex-tmp/cleanup-20260908-object-files.json)。
首次预检因 Python 3.8 缺 is_relative_to 退出，尚未删除对象；使用 relative_to
完成同等包含验证后成功，首边界已记录 failure-log。

清理后当前 C++ batch 26 cases/546 assertions PASS；见
[R1-B2](r1-b2-role-semantics-20260908.md)。本项不关闭任何产品资格任务。
