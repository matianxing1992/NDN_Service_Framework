# Tiger baseline identity locator repair

## Scope

本记录覆盖现有 `Experiments/TigerCluster` 运行器的身份绑定修正，不重写
SIF 构建或 APP 打包流程。base `62d5ea0e` 上冻结的两个文件是：

- `Experiments/TigerCluster/runtime/baseline.py`
- `Experiments/TigerCluster/tests/test_baseline.py`

普通运行角色现在显式使用目录级 ndn-cxx locator：
`pib-sqlite3:/identities/<role>/.ndn` 与 `tpm-file:/identities/<role>/.ndn`。
`prepare` 身份签发命令省略这两个变量，使 `identities.py` 通过 issuer 的 `HOME`
使用独立 store。locator 不写入文件名，因为 backend 会追加 `pib.db` 和
`ndnsec-key-file`。

## Static gate

官方 `review-agent` 对 immutable diff snapshot 返回 `STATIC_PASS`，无 P0--P3：

- base: `62d5ea0e`
- diff SHA-256: `09e7ccd0cb8fd827e8fd9a94fc1fc8dcf8710c345558898921234794934e5d7e`
- reviewer scope: 完整 diff、`worker.py`、`jobs/baseline/submit.py`、
  `runtime/identities.py` 及 Runtime KeyChain locator 语义

审查覆盖 static/security、compile-link、runtime-test、compatibility/integration
和 unobserved 五 lane；审查员未执行构建、运行时或 Tiger/Slurm qualification。

## Focused validation

```text
pytest -q Experiments/TigerCluster/tests/test_baseline.py
53 passed in 0.41s
python3 -m py_compile Experiments/TigerCluster/runtime/baseline.py \
  Experiments/TigerCluster/tests/test_baseline.py
git diff --check -- Experiments/TigerCluster/runtime/baseline.py \
  Experiments/TigerCluster/tests/test_baseline.py
```

这些检查证明命令形状与 Python 语法；尚未证明有效 base SIF、容器内 KeyChain、
NFD socket、C++ 请求链或 TigerCluster 运行。现有 broken base-SIF symlink
边界仍保持在 `docs/failure-log.md`，因此本修正不构成 SIF/实验 PASS。
