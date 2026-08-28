# Spec 133 Implementation Progress

> **Historical checkpoint (2026-07-22), not current feature status.** This
> file preserves the rejected cross-thread/pre-recovery state and must not be
> used as the final completion authority. The current status is in
> `../spec.md`, `../tasks.md`, `bottleneck-report.md`, and
> `../checklists/post-implementation-audit.md`.

**Date**: 2026-07-22  
**Completed tasks**: T001--T008, T010  
**Blocked incomplete tasks**: T009, T011  
**Formal cells consumed**: 0

## T002 Clean Foundation

- Base commit: `a9944019f76791773604999f00128057b9534ace`
- Base tree: `945a321d473f44f29e8349a83ce60373f3e37420`
- Boost-only clean head: `bf1e3e37f0c4c7a5a04d678f0fa439283ee46d2d`
- Clean tree: `5fec9ee7aad4a124e34589d1fc5c5531bd4052ea`
- Clean library SHA-256:
  `6850881acbaa3699ed2e489298cfbb4e3d0917b3d413f89d9bbf04c024036ced`
- Canonical Boost patch SHA-256:
  `36c8d5429b0033c1350caebd6ab4fc1eacd0c35477845fec9644c083b6682307`
- Compiler-visible Boost: `107100` / `1_71`
- Generated config: `/* #undef NDN_SVS_COMPRESSION */`
- Internal `publishAsync`, parallel Sync, and library-owned thread audit: no
  matches.
- Active NDN-SVS ref remained `Experimental` at
  `6bb34545b4f89f1f6c265a68c18f1a40ade413eb`.

Authority: `build/spec133/subject-foundation.json`. This local build artifact is
not a final profiled-subject manifest.

## T003 Profiler Primitives

- Added the fixed registry, `CLOCK_MONOTONIC_RAW` RAII spans, exact atomic
  summaries, deterministic trace-key sampling, thread-local diagnostic trace
  scope, lifecycle records, and explicit Boost.Log flush.
- The runtime registry exactly matches every frozen stage ID in the measurement
  contract.
- Dedicated component: `NDN_LOG_INIT(ndn_svs.Profile)`.
- No logging worker, thread, `boost::asio::post`, or protocol task was added.

Verification:

```text
python3 tests/python/test_spec133_svs_sync_stage_profile.py
12 tests: PASS
```

The focused test compiles `profile.cpp`, runs it with sample modulus 1, and
requires profile-start, sampled child/parent spans, all-stage summaries, and
profile-stop. It was repeated three times after the flush fix.

## T004 Publisher And Sync Production

The historical worktree was configured with tests, rebuilt using `-j2`, and
run against its own library:

```text
LD_LIBRARY_PATH=<profile-worktree>/build ./build/unit-tests
10 test cases: PASS
```

Running without `LD_LIBRARY_PATH` loaded a separately installed ABI-incompatible
NDN-SVS library and failed symbol resolution. The accepted test command binds
the worktree library explicitly; `ldd` confirmed that path and Boost 1.71.

The enabled focused smoke produced nonzero, same-trace spans/summaries for:

- `PUB.TOTAL`
- `PUB.INNER_SIGN`
- `PUB.OUTER_SIGN`
- `MAP.STORE_INSERT`
- `SYNC.MAPPING_CANDIDATE_ENCODE`

Raw local diagnostic log:
`build/spec133/t004-profile-smoke.log`.

## T005 Receive, Mapping, And Payload Paths

- Sync receive now separates validation, ApplicationParameters parsing,
  VersionVector decode, extra Mapping/piggyback decode, vector-lock wait,
  merge, update dispatch, suppression work, recorded-vector lock wait, and
  scheduler lock wait. Async validation duration retains the receive trace;
  the formal null/HMAC paths remain synchronous.
- Mapping processing separates local lookup, freshness, subscription matching,
  existing piggyback-cache lock wait, cache lookup, fetch-queue insertion, and
  piggyback callback. No mutex was added to the historical unsynchronized
  Mapping containers.
- Mapping fallback separates Interest build, Fetcher queue wait, Face express,
  exact express-return-to-Data/Nack/timeout wait, provider query/lookup/encode/
  build/sign/put, validation, content extraction, decode, and remote insertion.
- Payload fallback separates Interest build, Fetcher queue wait, Face express,
  exact external wait with success/Nack/timeout outcomes, provider store-find/
  put, outer validation/cache, inner decode/validation, and library callback.
- Disabled validation produces explicit `outcome=skipped`; Nack, timeout, miss,
  and invalid branches use explicit outcomes. Cross-peer Sync wait is not
  fabricated by the source patch: it remains a later analyzer join and cannot
  contribute a duration unless a unique existing-wire occurrence match exists.
- Diagnostic timing metadata in `fetcher.hpp` is local-only. It adds no API
  argument, name component, packet field, or wire-visible trace identity.

Verification:

```text
./waf build -j2
PASS

LD_LIBRARY_PATH=<profile-worktree>/build ./build/unit-tests
10 test cases: PASS

python3 tests/python/test_spec133_svs_sync_stage_profile.py
14 tests: PASS
```

The runtime fixture covers Mapping and Payload success, Payload Nack and
timeout, Mapping provider response production, Payload provider store service,
outer/inner disabled validation, subscription delivery, and a complete Sync
receive/merge/update/suppression path. The profile-enabled historical unit
fixture also emitted nonzero `SYNC.EXTRA_MAPPING_DECODE`,
`SYNC.PIGGY_DATA_DECODE_CACHE`, `MAP.PROCESS_TOTAL`,
`MAP.FETCH_QUEUE_INSERT`, and `PAYLOAD.SUBSCRIPTION_CALLBACK` summaries.
Raw local diagnostic log: `build/spec133/t005-profile-unit.log`.

## T006 Shared Synchronous Driver

**Superseded / must be replaced**: the implementation below is preserved as
historical work, but the NDN-SVS source-contract audit found that it performs
unsupported cross-thread calls. T006 is now unchecked.

- Added one source file compiled against both the clean and profiled historical
  libraries. `SPEC133_PROFILED` controls diagnostics access only; both binaries
  execute the same synchronous workload logic.
- The application main thread directly calls `SVSPubSub::publish()`. The only
  created thread owns `Face::processEvents`; there is no publisher thread,
  scheduler-generated load, pacer, adapter, async call, or worker pool.
- The driver validates subject/profile mode plus cell/peer environment identity,
  emits immutable context, process/thread, deadline, API enter/return/error,
  state-update, invalid/duplicate/delivery, and process-stop events using
  `CLOCK_MONOTONIC_RAW`.
- The profiled build emits `APP.PAYLOAD_CHECK`, `APP.STATE_UPDATE`, and
  `APP.DELIVERY`; it explicitly flushes the profiler only after Face shutdown
  and Face-thread join.
- The in-process two-peer self-test has two independent scenarios: a complete
  piggyback path and a forced one-byte extra/piggy limit that exercises both
  Mapping fallback and Payload fallback. This is a diagnostic fixture, not a
  MiniNDN or formal cell.

Verification:

```text
clean historical library + shared driver --self-test:
SPEC133_SELF_TEST_OK ... piggyback=1 fallback=1 profiled=0

profiled historical library + shared driver --self-test:
SPEC133_SELF_TEST_OK ... piggyback=1 fallback=1 profiled=1

python3 tests/python/test_spec133_svs_sync_stage_profile.py
15 tests: PASS

LD_LIBRARY_PATH=<profile-worktree>/build ./build/unit-tests
10 test cases: PASS
```

The profiled self-test produced nonzero summaries for
`MAP.PIGGY_CALLBACK`, `MAP.NETWORK_WAIT`, `PAYLOAD.NETWORK_WAIT`,
`PAYLOAD.SUBSCRIPTION_CALLBACK`, `APP.PAYLOAD_CHECK`, `APP.STATE_UPDATE`, and
`APP.DELIVERY`. Raw local diagnostic log:
`build/spec133/t006-profile-selftest.log`.

## T007 Runner Contract

**Superseded / must be updated**: the runner launches two correct MiniNDN
processes, but the peer binary uses the invalid cross-thread execution model.
T007 is now unchecked.

- Added a lazy-import MiniNDN runner with a fixed A/B/C overhead triplet:
  clean control, profiled library with profiling disabled, and the same
  profiled library with formal logging enabled. The comparison receipt reports
  A-vs-B, B-vs-C, and A-vs-C attempted-rate, delivery-ratio, and aggregate CPU
  deltas against the frozen 5% gates.
- Profile-enabled admission requires one lifecycle start/stop per peer, exactly
  the frozen number of stage summaries, matching cell/peer/sample identities,
  and zero dropped records. A schema/log failure rejects admission.
- The formal manifest constructor emits exactly five ascending once-only cells
  at 200/400/600/800/1000 pps per peer, two symmetric peers, 10/60/10 timing,
  256-byte payload, fixed zero-loss topology, and the profiled binary only.
- Seal validation binds subject manifest, overhead receipt, runner, driver,
  binaries, and libraries by SHA-256. The runner uses a process-level nonblocking
  file lock, rejects any pre-existing cell/receipt, and emits one immutable
  `COMPLETE`, `SUBJECT_FAILURE`, or `INFRA_INVALID` terminal receipt.
- Resource sampling resolves the actual driver descendant rather than charging
  the shell wrapper. Raw app events, profile logs, resource samples, NFD packet
  capture, commands, topology, and environment are preserved per cell.

Verification:

```text
python3 -m py_compile Experiments/NDN_SVS_Sync_Stage_Profile_Minindn.py
PASS

python3 tests/python/test_spec133_svs_sync_stage_profile.py
20 tests: PASS
```

The runner was not invoked against MiniNDN. No overhead arm, campaign manifest,
seal, receipt, or formal cell was created by T007.

## T008--T011 Blocked State

- T010 added the strict analyzer and fail-first accounting fixtures. The final
  suite contains 24 passing tests.
- T008 froze profiling commit
  `e9913c9a957a214d699ab5eb0bc99684e06573c5`, rebuilt both independent
  subjects, verified Boost 1.71 linkage, and passed both same-driver self-tests.
- The one authorized MiniNDN overhead preflight returned `REJECTED`: all three
  arms ended in `SUBJECT_FAILURE`, including clean control. Evidence is in
  `results/spec133-svs-sync-stage-profiling/preflight-01/`.
- Per FR-009, no formal manifest was sealed and T009 consumed zero formal cells.
- T011 remains incomplete. An explicit inconclusive report and
  post-implementation `BLOCK` document the missing formal evidence; no rate
  table was synthesized from incomplete preflight logs.
- After the NDN-SVS README/header/example/test audit, T008 is also reset to
  incomplete: its frozen build remains valid historical evidence, but its
  preflight cannot admit the revised single-I/O-thread experiment.

Final verification:

```text
python3 tests/python/test_spec133_svs_sync_stage_profile.py
24 test cases: PASS

LD_LIBRARY_PATH=<profile-worktree>/build ./build/unit-tests
10 test cases: PASS

Spec Kit structural audit
17 FRs, 8 SCs, 3 user stories, 6/11 tasks complete; T006-T009/T011 blocked
```

The remaining technical question is outside frozen Spec 133: diagnose the
historical synchronous subject's bidirectional heap corruption under load in a
new Spec 134 without rerunning or relabeling the Spec 133 gate.
