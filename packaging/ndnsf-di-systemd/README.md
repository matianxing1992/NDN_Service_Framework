# NDNSF-DI Local MiniNDN Operator Runbook

This package operates the Spec 105 local CPU/ONNX MiniNDN candidate. It is not
a physical-production release: GPU hosts, physical networking, real production
identities, and cross-host acceptance are exclusively owned by Spec 106.

## 1. Identity and configuration

Create controller, provider, user, and Repo identities inside the MiniNDN node
namespaces. Install certificates and the trust schema under
`/etc/ndnsf-di/`; private PIB/TPM files must be owned by the service account,
mode `0600`, and referenced by path only. Copy the `.example` files from
`config/`, choose unique provider instance numbers, and replace every sample
name and path. Never place keys, tokens, or passwords in environment files.

Validate before installation:

```bash
ndnsf-di doctor --profile /etc/ndnsf-di/deployment.json --json
```

The doctor must report the expected identity, NFD socket, certificate, trust
schema, CPU ONNX backend, model manifest, writable directories, lifecycle
bounds, disk/permission state, and fresh Linux telemetry source. A failure is a
stop condition.

## 2. Build and release

Create an immutable release with only allowlisted artifacts:

```bash
packaging/ndnsf-di-systemd/create-release.sh \
  --output /tmp/ndnsf-di-r1 --release-id spec105-r1 \
  --artifact build/examples/di-native-provider:bin/di-native-provider \
  --artifact build/examples/App_ServiceController:bin/App_ServiceController \
  --artifact examples/ndnsf-di-qwen-pilot.model.json:share/model-manifest.json
sudo packaging/ndnsf-di-systemd/install.sh --release /tmp/ndnsf-di-r1
```

Installation verifies `SHA256SUMS`, preserves `/var/lib/ndnsf-repo`, installs
under `/opt/ndnsf-di/releases/`, and atomically changes `current`. Run
`systemd-analyze verify` on the installed units and `systemd-tmpfiles --create`
for the supplied tmpfiles definition before start.

## 3. Start, status, and canary

```bash
sudo systemctl start ndnsf-di-controller.target
sudo systemctl start ndnsf-di-provider@0.service ndnsf-di-providers.target
ndnsf-di status --profile /etc/ndnsf-di/deployment.json --json
ndnsf-di metrics --profile /etc/ndnsf-di/deployment.json \
  --format prometheus-textfile --out /var/lib/node_exporter/textfile/ndnsf-di.prom
sudo systemctl start ndnsf-di-bench.service
```

Run the canary twice from different empty `results/spec105-local-canary-*`
directories. Each run records source commit, release/profile/plan/evidence
digests, host facts, CPU backend, MiniNDN topology, exact command, and every
failure. Do not retry or reuse an output directory.

## 4. Restart, upgrade, and rollback drill

Capture status and metrics, stop one provider, confirm bounded epoch-1 recovery
or one exact terminal failure, then start it and verify a new provider boot ID.
Build release N+1 with a new ID, install it, restart providers one at a time,
and verify plan/evidence compatibility before traffic. Test an intentionally
incompatible cache binding: it must rebuild from full context or fail explicitly.

Rollback preserves Repo/catalog state:

```bash
sudo packaging/ndnsf-di-systemd/rollback.sh
sudo systemctl restart ndnsf-di-controller.target ndnsf-di-providers.target
```

Confirm `current`/`previous` digests and Repo data before and after. Disposable
model, activation, and KV cache may be cleared only through an explicit
disposable-cache path; never include `/var/lib/ndnsf-repo`.

## 5. Evidence and soak

Collect `systemctl show`, `journalctl --output=json`, doctor/status/metrics JSON,
release manifest/digests, canary summaries, lifecycle CSV, and sampled timeline
traces. Redact and negative-scan all bundles for keys, tokens, payloads, and
private paths. INFO is the operational level; TRACE is not acceptance evidence.

The frozen 24-hour, 1 RPS soak runs only if the immutable T062 performance gate
is PASS. If it is BLOCK, record `NOT RUN / BLOCK` and the controlling evidence;
do not reduce the rate, shorten the window, or create a replacement run.

## 6. Emergency stop and uninstall

```bash
sudo systemctl stop ndnsf-di-bench.service ndnsf-di-providers.target \
  ndnsf-di-controller.target
sudo packaging/ndnsf-di-systemd/uninstall.sh
```

Use `--purge-disposable-cache` only after confirming its scope. Uninstall
removes activation links and optionally the DI cache, but always preserves the
authoritative Repo. Preserve logs and failed evidence before cleanup.
