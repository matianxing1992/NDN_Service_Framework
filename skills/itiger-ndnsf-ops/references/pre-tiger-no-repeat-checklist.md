# Choose the next check from the first failure

| First failed boundary | Smallest useful check |
| --- | --- |
| SSH | Verify explicit account, DNS/route, TCP, host key, authentication and remote command in order. |
| Slurm pending or node unavailable | Check exact owned job, reason, current eligible nodes and reservations. Reconcile before submitting again. |
| Artifact missing or quota | Compare the published absolute path and digest with the consuming operation. Recover existing verified bytes before rebuilding. |
| Native library or SDK mismatch | Check actual installed header, build flags, ELF and loader paths, then run the exact installed consumer. |
| Graph or CUDA admission | Inspect real operators, dtype, I/O shape, role closure, device and provider profile. Reproduce the affected boundary on an allocated node. |
| Warm placement | Check authenticated lease, original role/device/runner, complete and incremental budgets, fresh signed capability, and requester cache lifetime separately. |
| Greedy token mismatch | Use the earliest divergent token under the same accepted prefix. Measure actual candidate logits and isolate the relevant operation before changing precision. |
| CPU oracle pending or failing | Check physical node/boot/release binding, source identity, staging state, checkpoint expiry and comparison extent. |
| Terminal or cleanup unknown | Query the original Slurm job and both original nodes' host and cgroup/role receipts. Keep unresolved evidence. |

Retain first failure, source/configuration, actual command and exit status. A
deterministic failure needs a changed input, a repair or a narrower probe before
another expensive run. Use bounded backoff for a confirmed transient condition.
