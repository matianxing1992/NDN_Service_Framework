# T002 Qualification Gate

**Verdict**: PASS

The fresh qualification campaign is:

`results/spec139-svs-fixed-worker-proof/confirmation01-20260723`

Both modes used the same frozen binary
`c4f3b296137033eb82d0e888bd2cacdf492a0e06609de12c3e1e301547e435ac`
at NDN-SVS commit `6bb34545b4f89f1f6c265a68c18f1a40ade413eb`.
Only the registered production-path fields differ. `worker-serial` used exactly
one production worker and one active signer; receive workers remained disabled.

| Mode | Attempted pps | Rate error | Delivery ratio | Face production CPU | Heartbeat p99 | Active signers |
|---|---:|---:|---:|---:|---:|---:|
| Face serial | 594.067 | 0.989% | 1.000 | 12.835% | 1.541 ms | 1 |
| One worker serial | 593.467 | 1.089% | 1.000 | 5.459% | 1.885 ms | 1 |

For both cells, production fallback was zero, production and publication
accounting remainders were zero, ownership checks passed, resource sampling was
complete, and shutdown drained. The Face-serial control exceeded the
pre-registered 10% Face-production pressure threshold.

The campaign contains no formal receipts. Qualification only establishes that
600 pps is an admissible and relevant boundary; it is not formal evidence for
the necessity predicate.

Validation:

- Python syntax checks: PASS
- focused unit tests: 6/6 PASS
- strict Spec Kit structural audit: PASS
- qualification checks: 8/8 PASS
