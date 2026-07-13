# NDNSF-DI 本机 MiniNDN 运维手册

本包只操作摘要绑定的 Spec 107 本机 CPU/ONNX MiniNDN 候选。GPU 主机、物理网络、
生产身份、真实 systemd 管理器和跨主机验收属于 Spec 106；所有本机记录必须保持
`physicalProductionDeferred=true`，不得据此宣称物理部署就绪。

配置文件只能引用不可变 release 内的相对 packaged command，并包含
`candidateId`、`planDigest`、`releaseRoot`、进程名和 readiness marker。私钥、token、
payload、tensor 和 KV 内容不得进入配置或证据。

两次 canary 必须使用两个全新的目录，不能重试或复用：

```bash
packaging/ndnsf-di-systemd/run-local-supervised.sh canary \
  --config /tmp/spec107-supervisor.json \
  --staging-root /tmp/spec107-canary-1/staging \
  --output /tmp/spec107-canary-1/canary.json --restart

packaging/ndnsf-di-systemd/run-local-supervised.sh canary \
  --config /tmp/spec107-supervisor.json \
  --staging-root /tmp/spec107-canary-2/staging \
  --output /tmp/spec107-canary-2/canary.json --restart
```

本机监督类型固定为 `local-process-supervision`。命令会验证 packaged executable、
等待 readiness、记录 PID/进程组/boot/command digest，执行可选重启，并在退出前证明清理。

N→N+1→N 回滚演练使用：

```bash
packaging/ndnsf-di-systemd/run-local-supervised.sh operations \
  --root /tmp/spec107-operations/root \
  --release-n /tmp/spec107-release-n \
  --release-n1 /tmp/spec107-release-n1 \
  --output /tmp/spec107-operations/operations.json
```

激活与回滚必须保持 `/var/lib/ndnsf-repo` 摘要不变。plan/candidate 绑定不兼容时只能
删除 `/var/cache/ndnsf-di` 中的 disposable cache，禁止删除 authoritative Repo。

静态 staging 校验：

```bash
packaging/ndnsf-di-systemd/validate-staging.sh \
  --work-root /tmp/spec107-staging-validation \
  --candidate-id "$SPEC107_CANDIDATE_ID" \
  --plan-digest "$SPEC107_PLAN_DIGEST"
```

输出必须包含 release、plan、candidate、provider boot、queue、request、terminal、
Repo digest 和 supervision identity。只有性能、恢复、运维、磁盘和输出独占门禁全部
PASS，T063 才能授权一次不可替换的 24 小时、1 RPS soak；否则记录
`SOAK_NOT_ELIGIBLE`，不得缩短、降载或补跑。
