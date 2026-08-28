# Experiment Contract

```text
commit = 6bb34545b4f89f1f6c265a68c18f1a40ade413eb
binary_sha256 = c4f3b296137033eb82d0e888bd2cacdf492a0e06609de12c3e1e301547e435ac
rate = 600 pps
modes = [face-serial, worker-serial]
worker_count = [0, 1]
qualification = 5/15/5
formal = 10/60/10
order = AB/BA/AB
formal_receipts = 6
automatic_retry = false
```

Qualification and formal admission:

```text
attempted_error <= 0.02
delivery_ratio >= 0.99
max_active_signers == 1
fallback == 0
accounting_remainder == 0
owner_violations == 0
pending == 0
```

The necessity predicate is FR-011 and cannot be overridden.
