# US1 Distributed Consistency Gate

Date: 2026-07-14  
Verdict: **PASS for the Spec 111 MiniNDN consistency gate**

## Candidate and scope

- Source baseline: `4d695ce8b7ffe2c79465dc1f3db649a5a65806a6` plus the recorded dirty-worktree candidate suffix in each result directory.
- Python extension digest prefix: `3a87d112`.
- Network/security/fault execution used MiniNDN only. No host/default NFD, OCI/SIF build, container runtime, Slurm or iTiger command was used.
- MiniNDN reports the repository's dummy-keychain test patch; therefore this gate proves protocol placement, authenticated-evidence plumbing and fail-closed state transitions, not production cryptographic strength.

## Deterministic contract tests

```bash
PYTHONPATH="$PWD/NDNSF-DistributedInference:$PWD/pythonWrapper" python3 tests/python/test_ndnsf_execution_lease_table.py -v
PYTHONPATH="$PWD/NDNSF-DistributedInference:$PWD/pythonWrapper" python3 tests/python/test_ndnsf_di_execution_lease_transaction.py -v
PYTHONPATH="$PWD/NDNSF-DistributedInference:$PWD/pythonWrapper" python3 tests/python/test_ndnsf_di_execution_consistency.py -v
PYTHONPATH="$PWD/NDNSF-DistributedInference:$PWD/pythonWrapper" python3 tests/python/test_ndnsf_di_requester_recovery.py -v
PYTHONPATH="$PWD/NDNSF-DistributedInference:$PWD/pythonWrapper" python3 tests/python/test_ndnsf_di_deployment_fencing.py -v
PYTHONPATH="$PWD/NDNSF-DistributedInference:$PWD/pythonWrapper" python3 tests/python/test_ndnsf_di_orphan_cleanup.py -v
```

Result: 30/30 passed. Covered prepare/commit/abort/release idempotency and expiry, authenticated Targeted Data evidence, exact receipt sets, stable certificate digests, partial-commit rejection, same-identity requester recovery, higher-attempt fencing, lifecycle CAS fencing, Provider boot changes and periodic no-new-traffic cleanup.

## Positive control

Command:

```bash
sudo -n env -u NDN_LOG PYTHONPATH="$PWD/NDNSF-DistributedInference" \
  python3 Experiments/NDNSF_DI_NativeTracer_Minindn.py \
  --full-network --core-trace --tracer-deterministic-runner \
  --enable-execution-leases --requests 1 --concurrency 1 \
  --skip-provider-pair-telemetry-probe \
  --out results/spec111-us1-core-4d695ce8b7ff-3a6bbdf23a87d112-base
```

- Status: `SUCCESS`; 1/1 request completed.
- All four Provider leases prepared, committed and activated under one certificate.
- Dependency objects: 4 publish + 4 fetch, all `status=ok`.
- Summary SHA-256: `43b37983906a41bfa8252b316bfafd73a3095c90bf16efe7aeac57dbd882b395`.

## Fault matrix

All commands add `--full-network --core-trace --tracer-deterministic-runner --enable-execution-leases --requests 1 --concurrency 1 --skip-provider-pair-telemetry-probe`.

| Cell | Exact extra arguments/result | Observed safety result | Summary SHA-256 |
|---|---|---|---|
| requester loss after PREPARE | `--spec111-fault requester-loss-after-prepare --out results/spec111-us1-fault-requester-loss-4d695ce8b7ff-f53138113a87d112` | requester exit `-15`; 8-second lease-expiry/periodic-sweep window; zero NativeTracer execution | `bf52c450b488a8fe43f72c2fb95b3a7bbbfbee847ffade17f7a191ad544d6952` |
| Provider restart after COMMIT | `--spec111-fault provider-restart-after-commit --out results/spec111-us1-fault-provider-restart-4d695ce8b7ff-f53138113a87d112` | old Backbone exit `-15`; replacement became ready with a new boot; request timed out; zero NativeTracer execution from the incomplete/old certificate | `294994d539797c7dabf7ade17f72044320dfda070cdc43e9dd5c9021901d5af9` |
| network cut after certificate gate | `--role-execution-delay-ms 1500 --spec111-fault network-cut-after-certificate --out results/spec111-us1-fault-network-cut-4d695ce8b7ff-8552d2923a87d112` | `memphis-ucla` down for 2 seconds then restored; exactly four certificate-gated role starts, no duplicate authority; request completed with 8/8 dependency events | `a1f38477c0f12dc37d4cba82eaa8e08eeb3015d0d4c2a89b22658e1e2b7450ac` |

Cleanup bound: the harness owns and stops every launched controller/provider/user process and MiniNDN network in `finally`; deterministic orphan tests additionally prove leases, reservations, sessions, cache pins, runner handles and bounded tombstones are reclaimed by the Provider-owned periodic sweep without a new operation.

## Preserved negative outcomes

- `results/spec111-us1-fault-requester-loss-4d695ce8b7ff-b3ee23473a87d112`: injection occurred, but the first harness version rejected the intentionally incomplete requester log before evaluating the safety invariant.
- `results/spec111-us1-fault-network-cut-4d695ce8b7ff-f53138113a87d112`: the first trigger matched NativeTracer REQUEST rather than certificate-gated execution; retained as a pre-certificate partition negative, not counted as the post-certificate cell.
- Earlier packet-size, plan-digest and typed-Name failures remain preserved in their distinct `results/spec111-us1-core-*` directories and were not rerun under the same candidate identity.

No negative result is relabeled as a successful inference run.
