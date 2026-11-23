# 通用 Repo 验收

本目录的 `controller.py` 和 `repo_policy.yaml` 供维护中的 C++ Repo MiniNDN 验收使用。
[原生验收入口](../../../../tests/minindn/run_spec192_real_cpp_repo.py) 启动实际的
`DistributedRepoServer`，通过现行客户端验证对象发布、取回、授权拒绝和重启恢复。
`OBJECT_BASELINE` 验证通用对象路径，`PERSIST_SEED` 和 `PERSIST_READ` 复用
同一持久目录验证重启；运行参数见入口的 `--help`。

原 Python Repo 示例和其 MiniNDN 启动器已退役。
