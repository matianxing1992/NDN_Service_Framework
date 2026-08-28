# Spec 144 Completion Summary

**Status**: **COMPLETE / MEASURED NEGATIVE**  
**Formal campaign**:
`results/spec144-uav-sensor-stream-20260724T165132Z`  
**Formal rerun permitted**: no

## Delivered

- generic unsampled Mapping/Payload terminal-attempt, future-hit, retry,
  timeout, Nack, source/repair, recovery, and Interest-utility status with
  native/Python binding parity;
- a guarded one-shot two-node MiniNDN runner and 32-cell manifest campaign;
- a real UAV-APP 20 Hz, 256/384/512-byte single-item telemetry stream while
  preserving `GetStatus`;
- a real UAV-APP 40 ms, variable 2/3/4-source acoustic stream using generic
  two-repair publication/recovery;
- deterministic, security, analyzer, runner, full build/binding, and
  preflight evidence;
- one immutable 32-cell formal campaign and independent workload reports.

## Verification Before Freeze

- native build PASS with `-j2`;
- forced Python binding rebuild PASS;
- native suite 375/375 PASS;
- first-future regression stress 50/50 PASS;
- Python suites 19/19, 27/27, 8/8, and 8/8 PASS;
- UAV stream security contract PASS with 127 native cases;
- telemetry and acoustic zero-loss MiniNDN preflights PASS;
- prohibited Core/binding selector scan: zero executable violations.

## Formal Result

| Workload | zero loss | loss | reorder | combined | Family verdict |
|---|---:|---:|---:|---:|---|
| telemetry | 1/1 | 5/5 | 4/5 | 2/5 | FAIL |
| acoustic/audio | 1/1 | 0/5 | 0/5 | 0/5 | FAIL |

The campaign contains all 32 declared unique cells, all are terminal, and all
were invoked exactly once. Thirteen cells passed. No failed cell was replaced,
and no threshold or analyzer interpretation was changed after freeze.

The strongest positive bounded observation is that telemetry passed zero
loss, 1% loss, and the preregistered reorder treatment, while acoustic passed
only zero loss. The combined telemetry profile and all acoustic impaired
profiles prevent both family-level and shared generality claims.

## Post-Formal Audit Limits

Two findings prevent positive implementation acceptance:

1. FR-011 is not implemented. Payload Data is exact-name signed but the
   telemetry/acoustic application bytes are not encrypted or bound as
   ciphertext to the stream/session/name identity.
2. The frozen derived recovery `successRate` divides recovered sources by
   recovery attempts and can exceed 100%; only its raw counters may be used.

Neither issue was repaired after freeze. Both are preserved for a new Spec.
They do not change the already-negative treatment decisions.

## Immutable Evidence

- campaign summary:
  `01e95d9e79ccf879829a02d981d451e1cd35582aca86b57944330b2cdc2df738`;
- manifest:
  `5f980d857fb66f70e1939e408f35c780b14d4c202aad226d844b469da0f6a517`;
- CSV:
  `c9fd955a7a5880888c91608ebfe3566d44501f3e280c4e9918c2bc0de1052a96`;
- frozen historical Spec 127/128 and promoted Spec 145 hashes: unchanged.

See `evidence/telemetry-formal.md`, `evidence/acoustic-formal.md`, and
`evidence/neutrality-audit.md` for the complete per-cell evidence.

## Closure

T001-T009 were executed. Task completion records execution and evidence
closure, not successful acceptance. The post-implementation audit is BLOCK
for any positive security or generality claim, and Spec 144 must not be
rerun or retroactively repaired.
