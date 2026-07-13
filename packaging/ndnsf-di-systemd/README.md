# NDNSF-DI Local MiniNDN Operator Runbook

This package operates a digest-bound Spec 107 local CPU/ONNX MiniNDN candidate. It is not
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

Replace every `REPLACE_*` field first. The doctor must report the expected
identity/certificate name and digest, NFD socket, trust/release/model/plan and
execution-evidence digests, CPU ONNX backend, writable directories, lifecycle
bounds, disk/permission state, and a measured fresh Linux telemetry file.
Configured freshness is not a probe. A failure is a stop condition.

Validate the isolated staging tree with the exact frozen identities:

```bash
packaging/ndnsf-di-systemd/validate-staging.sh \
  --work-root /tmp/spec107-staging-validation \
  --candidate-id "$SPEC107_CANDIDATE_ID" \
  --plan-digest "$SPEC107_PLAN_DIGEST"
```

## 2. Build and release

Create an immutable release with only allowlisted artifacts:

```bash
packaging/ndnsf-di-systemd/create-release.sh \
  --output /tmp/ndnsf-di-r1 --release-id spec107-r1 \
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

Run the canary twice from different empty `results/spec107-local-canary-*`
directories. Each run records source commit, release/profile/plan/evidence
digests, host facts, CPU backend, MiniNDN topology, exact command, and every
failure. Do not retry or reuse an output directory.

For the Spec 107 local-process gate, use a JSON config with `candidateId`,
`planDigest`, `releaseRoot`, and packaged relative commands. Never point it at
an executable outside the immutable release:

```bash
packaging/ndnsf-di-systemd/run-local-supervised.sh canary \
  --config /tmp/spec107-supervisor.json \
  --staging-root /tmp/spec107-canary-1/staging \
  --output /tmp/spec107-canary-1/canary.json --restart

packaging/ndnsf-di-systemd/run-local-supervised.sh operations \
  --root /tmp/spec107-operations/root \
  --release-n /tmp/spec107-release-n \
  --release-n1 /tmp/spec107-release-n1 \
  --output /tmp/spec107-operations/operations.json
```

Each output is exclusive. `supervisionClass=local-process-supervision` and
`physicalProductionDeferred=true` are mandatory: these commands do not test a
real systemd manager or physical hosts.

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

The frozen 24-hour, 1 RPS soak runs only if the immutable T063 eligibility gate
is PASS. If it is BLOCK, record `NOT RUN / BLOCK` and the controlling evidence;
do not reduce the rate, shorten the window, or create a replacement run.

## 6. Emergency stop and uninstall

```bash
sudo systemctl stop ndnsf-di-bench.service ndnsf-di-providers.target \
  ndnsf-di-controller.target
sudo packaging/ndnsf-di-systemd/uninstall.sh
```

Use `--purge-disposable-cache` only after confirming its scope. Uninstall
stops/disables installed targets, removes activation links, units, tmpfiles and
logrotate assets, and optionally removes the DI cache. It always preserves the
authoritative Repo and operator profiles. Preserve logs and failed evidence
before cleanup.
