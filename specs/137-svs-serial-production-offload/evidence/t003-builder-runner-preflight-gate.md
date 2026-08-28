# T003 Builder, Runner, Analyzer, And Preflight Gate

> **Superseded design evidence (2026-07-23):** This file preserves the rejected
> eight-exclusive-CPU admission attempt. It is not the current Spec 137
> requirement and cannot support the final claim. The corrected experiment
> uses the existing four-core host, one active publisher, one fixed receiver,
> and one publisher worker thread.

## Status

`BLOCKED_BY_HOST_CPU_CAPACITY`

T003 is not closed. The implementation, parser/admission/tamper/error tests,
one-binary build, and real two-process MiniNDN smoke are present, but the full
preflight is correctly inadmissible on the current four-CPU host. No pilot,
seal, or formal cell was started.

## Frozen Subject

- Base commit: `6bb34545b4f89f1f6c265a68c18f1a40ade413eb`
- Base tree: `cc110d89083d2c0d63cf74292f4dcd4fab8aa194`
- Measurement patch SHA-256:
  `f17d20817230f9dad75b9fb89276ece3e4ffa709e8bd41c9e8671f5af2f91ca2`
- Final binary SHA-256:
  `f915cb3ac2680de21eecc1b0fbe88ade2f1fa6f9591eaf766b56f388a01f89f7`
- Benchmark source SHA-256:
  `644ab24a96db7e96fca44d4c99323e6b6873ea2a61476f475dbb8b390c5737c0`
- NDN-SVS library SHA-256:
  `8c32170a77560077ec741b1498cddcdefb76b6c1cc14680124a74834baa11d3c`
- Subject manifest SHA-256:
  `aeec31d800ed3c7985f11807c262f41bb01db73827ed9a7339ea190d424ee237`
- Compiler-visible Boost: `1.71` (`BOOST_VERSION=107100`)
- Active NDN-SVS checkout after build: exact base commit and clean.

The builder admitted only `wscript`'s canonical Boost-1.71 substitutions and
the five reviewed measurement-patch paths. It rejected Boost-1.74 linkage and
preserved all NDN-SVS worktrees outside `build/spec137/`.

## Tests

Command:

```bash
SPEC137_BENCH_BINARY=build/spec137/bin/svs-serial-production-offload \
  python3 tests/python/test_spec137_svs_serial_production_offload.py
```

Result: `13/13 passed`.

The tests cover exact base/patch allowlists, artifact-hash tamper rejection,
runner parser fail-closure, the AB/BA/AB matrix, duplicate ordinal rejection,
deterministic rate selection, event/schema/lifecycle validation, conservation,
fallback/shutdown gates, and both modes from the built single binary.

## Real MiniNDN Preflights

All three attempts are preserved and are not eligible for selective rerun:

1. `results/spec137-svs-serial-production-offload/t003-preflight-20260723-01`
   exposed mixed warmup/measured skipped-release accounting and an invalid
   saturated instrumentation rate.
2. `results/spec137-svs-serial-production-offload/t003-preflight-20260723-02`
   exposed a phase-boundary off-by-one because publication phase used actual
   wake time instead of the scheduled deadline.
3. `results/spec137-svs-serial-production-offload/t003-preflight-20260723-03`
   is the final current-code attempt.

The third preflight passed:

- source/binary hashes and Boost-1.71-only linkage;
- same-binary runtime treatment diff;
- no-op 1000 pps pacer: `994.0326 pps`, `0.5967%` error;
- two MiniNDN nodes, two concurrent PubSub processes, routes, and delivery;
- one Face thread and zero receive workers per process;
- `max_active_sync_signers=1`;
- zero fallback and zero production/publication accounting remainder;
- complete event/resource files; and
- drained shutdown with no pending work.

It failed exactly:

- `cpu_affinity_valid=false`: only CPUs `0-3` are available. A worker-mode
  cell needs six exclusive application CPUs plus two exclusive NFD CPUs.
- `instrumentation_overhead_within_budget=false`: the CPU-oversubscribed
  disabled/enabled arms attempted only `108.2/110.4 pps` at a `200 pps`
  target. Relative throughput and heartbeat costs (`0%` and `1.5748%`) cannot
  be admitted until both arms first reach the target within `+/-2%`.

`formalReceiptsCreated=0` in every preflight. There is no pilot, seal, campaign
manifest, or formal receipt ledger in the final campaign.

## Resume Condition

Use a host/cpuset with at least eight exclusive CPUs. Create a fourth, entirely
new campaign directory and rerun `--preflight` once. Do not modify, delete, or
rerun any preserved preflight attempt. T004 may begin only if every new
preflight check is true.
