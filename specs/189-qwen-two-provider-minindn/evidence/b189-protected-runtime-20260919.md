# B189 Protected Runtime Publication Evidence — 2026-09-19

## Scope

本单元只验证 Runtime 默认 Core publisher 与 `RepoEncryptedLargeDataStore` 的受保护
发布边界：prepare 使用 fixture-owned spool，Repo 保存 ciphertext envelope，Core 返回的
canonical manifest 保持明文身份，source owner 在 prepare 返回前释放。它不证明 ACK、
Selection、Provider assembly、Qwen 执行或 MiniNDN 资格。

## Static gate

官方只读 `review-agent` 已对 selector、异步 prepare/Face pump、fixture owner、超时关闭、
Repo bounded read 和 root/source identity 断言返回 `STATIC_PASS`。审查期间发现并修复：

- fixture 绑定替换了 Runtime 的 `ServiceUser`，测试随后在真实绑定 user 上重新安装同一个
  protected store；
- source 生命周期改为验证 weak owner 已过期；
- root ciphertext 不再直接解析为 JSON，root schema/state/source identity 改从
  `canonicalManifestJson` 校验；
- 大对象读取改为固定 1024-byte `getRange()`，不调用 backend 禁止的整对象 vector API；
- timeout 先关闭 Runtime 并继续有限 Face pump，再 join worker；
- staging spool 使用 fixture 私有目录并固定 owner-only 权限。

## Runtime attempts

`protected-runtime-r1` 是同步 prepare 版本，在等待借用 Face 时没有稳定出口；
`protected-runtime-r2` 在受保护发布 staging 处因历史 root-owned
`/tmp/ndnsf-large-data` 返回 `Permission denied`；`protected-runtime-r3` 使用 fixture
spool 后已完成受保护发布但 oracle 在
`RepoCore::get()` 触发 `repo-large-object-vector-path-disabled`。这些是测试/环境边界，
没有被计为产品协议失败或 PASS。

修复 bounded range oracle 后，使用同一已配置的 global-r3 build tree 重新构建：

```text
../waf build --targets=spec189-prepared-request -j1  PASS (29.085s; r6; material receipt assertions)
spec189-prepared-request --run_test='Spec185PreparedRequest/Spec189*'  PASS (2 cases)
```

运行从仓库根目录执行，并设置 build-tree/ONNX/global loader 路径。两个 C++ cases
均通过：plain provider case 证明同一 PreparedModel 的两次 request 不新增 Repo
publication；protected case 证明 Runtime 默认 Core publisher 将 source 与 root 写为
`encrypted-large-data-envelope`，使用 bounded `getRange()` 可读，source owner 在
prepare 返回后释放，canonical root receipt 的 schema/state/source identity 正确，第二次
prepare 不增加 Repo object 数量。新增断言还核对了 root receipt 中的 material manifest
name/digest、每个 payload 的 id/name/digest，并对 material manifest 与全部 payload 各做一次
最多 1024-byte 的 bounded `getRange()`；因此本地出口同时证明了材料对象的 Repo 可达性和
receipt 绑定。这个 selector 仍是小 fixture 的 protected publication
边界，不包含 ACK/Selection、Provider assembly、真实 Qwen 大对象或 MiniNDN 资格。

原始日志为 `.codex-tmp/spec189-b189-2-selectors-20260919/prepared-request-build-r6.log`
与 `.codex-tmp/spec189-b189-2-selectors-20260919/protected-runtime-material-batch-r1.log`；
单独 protected case 日志为
`.codex-tmp/spec189-b189-2-selectors-20260919/protected-runtime-material-r1.log`。

## Closure

当前状态 `PARTIAL / LOCAL_PROTECTED_PUBLICATION_PASS`。本地 protected publication 子
单元已通过，但 Spec189 的 T003、T005、T006、T007、T009 仍保持 `PARTIAL`。
