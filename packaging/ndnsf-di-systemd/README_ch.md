# NDNSF-DI 本机 MiniNDN 运维手册

本包只用于 Spec 105 的本机 CPU/ONNX MiniNDN 候选。它不是真机生产发布：
GPU 主机、物理网络、真实生产身份和跨主机验收全部由 Spec 106 独占管理。

## 1. 身份与配置

在 MiniNDN 节点命名空间内创建 controller、provider、user 和 Repo 身份。
证书与 trust schema 放在 `/etc/ndnsf-di/`；私有 PIB/TPM 文件必须归服务
账号所有、权限 `0600`，且只能以路径引用。复制 `config/` 下的 `.example`
文件，设置唯一 provider 实例号并替换示例名称和路径。环境文件禁止写入
私钥、token 或密码。

安装前执行：

```bash
ndnsf-di doctor --profile /etc/ndnsf-di/deployment.json --json
```

doctor 必须验证身份、NFD socket、证书、trust schema、CPU ONNX 后端、模型
manifest、可写目录、生命周期边界、磁盘/权限和新鲜 Linux telemetry 源；
任一失败都必须停止。

## 2. 构建与发布

只加入 allowlist 工件，生成不可变发布：

```bash
packaging/ndnsf-di-systemd/create-release.sh \
  --output /tmp/ndnsf-di-r1 --release-id spec105-r1 \
  --artifact build/examples/di-native-provider:bin/di-native-provider \
  --artifact build/examples/App_ServiceController:bin/App_ServiceController \
  --artifact examples/ndnsf-di-qwen-pilot.model.json:share/model-manifest.json
sudo packaging/ndnsf-di-systemd/install.sh --release /tmp/ndnsf-di-r1
```

安装程序校验 `SHA256SUMS`，保留 `/var/lib/ndnsf-repo`，发布安装到
`/opt/ndnsf-di/releases/`，再原子切换 `current`。启动前对 unit 执行
`systemd-analyze verify`，并用 `systemd-tmpfiles --create` 创建目录。

## 3. 启动、状态与 canary

```bash
sudo systemctl start ndnsf-di-controller.target
sudo systemctl start ndnsf-di-provider@0.service ndnsf-di-providers.target
ndnsf-di status --profile /etc/ndnsf-di/deployment.json --json
ndnsf-di metrics --profile /etc/ndnsf-di/deployment.json \
  --format prometheus-textfile --out /var/lib/node_exporter/textfile/ndnsf-di.prom
sudo systemctl start ndnsf-di-bench.service
```

在两个不同的空 `results/spec105-local-canary-*` 目录各执行一次 canary。
每次记录 source commit、release/profile/plan/evidence digest、主机事实、CPU
后端、MiniNDN 拓扑、完整命令及所有失败；禁止重试或复用输出目录。

## 4. 重启、升级与回滚演练

先保存 status/metrics，停止一个 provider，确认只发生一次 epoch-1 恢复或
一个精确终止结果；重新启动后必须看到新 boot ID。用新 ID 构建 N+1，安装
后逐个重启 provider，并在接流量前验证 plan/evidence 兼容性。注入不兼容
cache binding 时，只能从 full context 重建或显式失败。

回滚不得改变 Repo/catalog：

```bash
sudo packaging/ndnsf-di-systemd/rollback.sh
sudo systemctl restart ndnsf-di-controller.target ndnsf-di-providers.target
```

前后核对 `current`/`previous` digest 和 Repo 数据。模型、activation、KV cache
只有在明确限定 disposable-cache 范围时才能清理，绝不能包含
`/var/lib/ndnsf-repo`。

## 5. 证据与 soak

收集 `systemctl show`、JSON journal、doctor/status/metrics、release manifest、
digest、canary summary、lifecycle CSV 和采样 timeline。证据包必须对私钥、
token、payload 和私有路径做脱敏及负向扫描。运行日志使用 INFO；TRACE 不能
作为验收数据。

只有不可变 T062 性能门禁为 PASS，才能执行固定 24 小时、1 RPS soak。
若为 BLOCK，则记录 `NOT RUN / BLOCK` 及控制证据；禁止降低速率、缩短窗口
或创建替代运行。

## 6. 紧急停止与卸载

```bash
sudo systemctl stop ndnsf-di-bench.service ndnsf-di-providers.target \
  ndnsf-di-controller.target
sudo packaging/ndnsf-di-systemd/uninstall.sh
```

只有确认范围后才能加 `--purge-disposable-cache`。卸载只移除激活链接并可选
清理 DI cache，始终保留权威 Repo。清理前必须保存日志和失败证据。
