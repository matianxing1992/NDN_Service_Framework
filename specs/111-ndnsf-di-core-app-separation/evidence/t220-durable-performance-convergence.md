# T220 Durable Performance Convergence

Date: 2026-07-15  
Verdict: **FOCUSED GATES PASS; FORMAL CANDIDATE WITHHELD**  
Scope: one local mock-network microbenchmark and exactly one isolated 70-request
MiniNDN treatment diagnostic. No baseline cell, candidate identity, 20-cell
matrix, container runtime, Slurm or iTiger activity was started.

## Correctness-preserving optimization

The protected-envelope commit retains AES-256-GCM, exclusive/no-follow
temporary creation, atomic replacement and completion-before-return durability.
File-data and spool-directory syncs now execute concurrently after replacement;
both must complete before the caller continues. The journal transaction remains
durable. First-time key creation separately syncs the exact 32-byte key and the
new state-root/identity/child directory entries, and restart rejects a truncated
key.

Relevant source SHA-256 values:

- `runtime_journal.py`: `098b02f3582546c935457c256062ba47a65acb1d283c5bcc216b7f8cf045d637`;
- `app_sdk/client.py`: `3cd8a06966f5d4a99154bb290f52e8ad6f95801d6adae10d33ac3d1dad1734ef`;
- `app_sdk/contracts.py`: `abee1ff59bb2971992b349189fd1c519023b29c565229d9866b14b00eb1ba653`;
- LLM pipeline User: `6fd4483067c7d294f94d42c00217c067b50b3692a77e8dff868d4141cdd51cf7`.

## One bounded local microbenchmark

The bounds were fixed before measurement: first/last 50-request median drift
at most 15%, p50 at most 10 ms and p95 at most 20 ms. Command:

```bash
python3 tools/spec111/benchmark_durable_submit.py \
  --warmup 20 --requests 300 --edge-window 50 \
  --max-drift-percent 15 --max-p50-ms 10 --max-p95-ms 20
```

The single run completed 300/300 and returned **PASS**:

- p50/p95: `3.864662 / 6.367431 ms`;
- first/last 50 medians: `3.993670 / 4.387176 ms`;
- absolute edge drift: `9.853242%`;
- final journal/spool usage: `2,190,112 bytes`;
- temporary state was removed on process exit.

Benchmark tool SHA-256:
`7198f8d152557dfee668c2e01db7b0a0dd40d43922cf9841f7e7de401cadafbe`.

## One isolated MiniNDN treatment diagnostic

Result root:

```text
results/spec111-core-app-separation/diagnostic-durable-parallel-fsync-treatment-04
```

The existing `run_runtime_readiness_preflight()` mechanism ran exactly 10
warmup plus 60 measured requests with seed 11100. Result: **70/70 PASS**, zero
fatal log findings, zero surviving MiniNDN/NFD processes and confirmed removal
of `/tmp/spec111-app-state-81709d41247b82e54bf5a590`.

For the 60 measured requests:

- outer treatment p50/p95: `129.0815 / 151.3892 ms`;
- outer first/last ten medians: `131.0075 / 129.4775 ms`;
- native network p50/p95: `122.5050 / 145.5785 ms`;
- durable APP overhead p50/p95: `6.0740 / 6.88995 ms`.

Evidence SHA-256 values:

- `readiness-result.json`:
  `630079f42126cff5c7d43166b71485978d1174895e0a1259e1fac3aaa37e0b7e`;
- `llm-pipeline-user-measured.csv`:
  `dfccd5129e55af2576c17393c7c4c0f614c7640eed76dd04768579ab5058093f`;
- command log:
  `559fd4bb213b05fb3c1366d78f32a69118705800e27f2c49f009ef5656afaf4e`.

## Stop decision

The previously recorded matched baseline readiness has p50/p95
`115.14 / 142.07 ms`. The new treatment diagnostic is approximately
`+12.1083% / +6.5596%`, already outside SC-007's unchanged 5% end-to-end gate.
This is a diagnostic prediction, not a formal paired result, but it is strong
enough that immediately spending a new candidate identity and a clean 20-cell
matrix would knowingly repeat a likely rejection. T220 therefore exercises its
"at most one" bound with zero new candidate/matrix and stops at the verified
diagnostic. T212 and T215 remain the controlling immutable formal negatives.

The next admissible performance action is a new correctness-preserving design
that reduces the two durable request/result commit costs, followed by another
explicitly authorized candidate. It must not restart or overwrite any existing
cell. Spec 111 completion and T213 remain blocked until one prospective
single-candidate matrix satisfies SC-007.
