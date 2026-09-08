# NDNSF-DI Local Deployment Candidate

## Decision

Spec 105 is complete as an evidence-backed **BLOCK**, not a deployment PASS.
The local CPU/ONNX implementation, bounded scheduler, attempt authority,
telemetry, systemd package and rollback tooling exist. The fixed 1 RPS Qwen
cell, live network recovery, canary, live operations and soak do not pass or did
not run. `physicalProductionOverall` remains `DEFERRED` to Spec 106.

## Architecture and ownership

- Core owns generic Request/ACK/Selection/Response, permissions, tokens, replay
  protection, large-data transport and generic lease facts.
- NDNSF-DI owns model/role plans, execution evidence, measured provider
  telemetry interpretation, bounded dependency waiting, attempt recovery,
  cache bindings and operator adapters.
- systemd packaging owns process users, filesystem permissions, release
  activation and rollback. Authoritative Repo data stays outside release/cache
  lifecycle at `/var/lib/ndnsf-repo`.
- Spec 106 alone owns physical GPU facts, physical networking, production
  identities and physical release authority.

## Production CLI contract

Simulated Runtime v1 commands exist only below `contract-smoke`. Production
commands either execute typed local planning/status logic or a no-shell command
array from the deployment/campaign profile:

```bash
ndnsf-di provider --profile provider.json
ndnsf-di plan --model model.json --providers providers.json \
  --out plan.json --explain plan-explain.json
ndnsf-di run --profile deployment.json --plan plan.json \
  --request request.json --out result.json
ndnsf-di bench --campaign campaign.json --out results/unique-run
ndnsf-di doctor --profile deployment.json --json
ndnsf-di status --profile deployment.json --json
ndnsf-di metrics --profile deployment.json \
  --format prometheus-textfile --out /path/ndnsf-di.prom
```

`provider_command` must consume `{profile}`. `run_command` must consume
`{profile}`, `{plan}`, `{request}` and `{out}`. A campaign `command` must consume
`{campaign}` and `{out}`. Command arrays execute directly without a shell;
missing placeholders, unknown placeholders, absent commands and unsupported
fields fail closed. If no campaign command is supplied, `bench` invokes the
real MiniNDN Qwen harness with `--runtime qwen-onnx-cpu-native`.

## Bound profile and health

A production profile binds identity and certificate name/digest, trust schema
digest, release ID/manifest digest, model digest, plan ID/digest, execution
evidence epoch/digest, NFD socket, writable directories and measured Linux
telemetry. Doctor requires a fresh `ndnsf-di-measured-telemetry-v1` sample from
`linux-proc`; configured age is not measured telemetry.

Status and metrics snapshots require their respective v1 schema, age within the
profile limit, and exact release ID, plan ID and evidence epoch. Missing/stale
or mismatched snapshots return nonzero; metrics never converts absence into an
empty successful export.

## Build and test

```bash
./waf build --targets=unit-tests,di-native-provider -j4
./build/unit-tests --catch_system_errors=no \
  --report_level=detailed --log_level=nothing
python3 -m unittest discover -s tests/python -p 'test_*.py'
./examples/run_security_regressions.sh
python3 Experiments/NDNSF_Run_Minindn_Quick_Checks.py
python3 tests/python/test_ndnsf_di_deployment_readiness.py
python3 tests/python/test_ndnsf_runtime_doctor.py
```

On the current development host (6 logical CPUs, 12 GB RAM), native builds use
`-j4` by default. Use `-j2` for a later invocation only after observing
sustained swap activity or desktop stalls; the historical commands and timings
in evidence records retain the parallelism that was actually used.

The final integrated run passed 242 C++ cases/45,688 assertions, then 407 maintained
Python tests with one environment skip after convergence, six security regressions and the
default MiniNDN Repo/DI/UAV quick suite. A scheduler completion/counter race
found by the full run was fixed and then passed 500/500 repetitions. The
standalone scheduler passed ASan/UBSan with 1,000 waits.

## Experiment and release evidence

The immutable T062 cell completed 25/60 generations (41.6667%), achieved
0.4167 RPS and measured distributed p95 20.17x the matched baseline. Two other
prespecified repetitions failed during artifact export because the filesystem
filled. No fourth repetition is allowed. T078 proves recovery contracts but
records `networkInjection=false`; it is not live fault evidence.

`specs/105-ndnsf-di-deployment-readiness/release-gate.json` checks the six
dimensions and binds every referenced evidence file by SHA-256. Its candidate
source commit and gate-generator commit are separate. Current verdicts are:

| Dimension | Verdict |
|---|---|
| Evidence integrity | PASS |
| Correctness | PASS |
| Performance | BLOCK |
| Application security/log hygiene | BLOCK |
| Recovery | BLOCK |
| Operations | BLOCK |
| MiniNDN candidate overall | BLOCK |
| Physical production overall | DEFERRED (Spec 106) |

## Install and rollback

Use `packaging/ndnsf-di-systemd/README.md`. Releases are digest checked and
activated by `current`/`previous` symlinks. Same-release activation preserves
the old rollback point. Real-root install creates/checks dedicated service
accounts and ownership. Uninstall stops/disables installed targets, removes
unit/tmpfiles/logrotate assets, optionally removes only the disposable DI cache,
and always preserves the authoritative Repo.
