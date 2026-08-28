# T002 Implementation And Test Gate

**Verdict**: PASS  
**Date**: 2026-07-23  
**Scope**: Same-binary runtime treatment and measurement implementation only.
No pilot or formal MiniNDN cell was started.

## Subject Isolation

- Pinned NDN-SVS base commit:
  `6bb34545b4f89f1f6c265a68c18f1a40ade413eb`
- Implementation worktree:
  `build/spec137/worktrees/serial-production-offload`
- Sanitizer worktree:
  `build/spec137/worktrees/serial-production-offload-asan`
- The active `/home/tianxing/NDN/ndn-svs` checkout remained clean at the
  pinned commit.
- The measurement patch applies cleanly to that checkout with
  `git apply --check`.

## Frozen T002 Artifacts

| Artifact | SHA-256 |
|---|---|
| `Experiments/ndn-svs-pubsub-benchmark/spec137-measurement.patch` | `f17d20817230f9dad75b9fb89276ece3e4ffa709e8bd41c9e8671f5af2f91ca2` |
| `build/spec137/bin/svs-serial-production-offload` | `225fc25c9fdff455103bca095151928256ef07b05785689db6e2f1a67b147477` |
| `build/spec137/worktrees/serial-production-offload/build/libndn-svs.so` | `d041714d475b13207bacacbb030289ec4455b80c444adc964e8c3ba20e7eb5cb` |

The binary and library hashes above are T002 development-gate artifacts, not
the T003 immutable campaign subject. T003 must rebuild once and freeze its own
manifest before any pilot.

## Implemented Mechanism

- One binary selects `face-serial` or `worker-serial` at runtime.
- Parallel Sync receive processing remains disabled in both modes.
- `worker-serial` uses exactly one production worker, queue capacity 4096,
  worker-side extension construction, worker-side encode, and worker-side V2
  HMAC Sync signing.
- Both modes use the same external application pacer and `publishAsync()`.
- Snapshot/admission and final `expressInterest()` remain Face-owned.
- Every Sync signing entry uses a common active/max signer probe.
- Snapshot, extra build, encode, sign, queue wait, worker service, result-post
  wait, Face finalization, Face CPU, worker CPU, and serial CPU are separately
  accumulated without overlapping snapshot and extra-build intervals.
- Queue-full fallback is visible and fail-closed.
- Serial failure, worker failure, Face-finalization failure, stale drop,
  completion, pending work, and cancellation-on-shutdown are explicit terminal
  counters.
- `SVSPubSub` stops and joins its production worker before destroying callback
  state used by extension construction.
- The standalone benchmark adds a 1 ms absolute Face heartbeat, symmetric
  256-byte bidirectional PubSub workload, runtime configuration record,
  terminal accounting, CPU affinity checks, and structured JSONL/resource
  output.

## Verification

### Optimized GCC Build

```text
python3 waf configure --enable-shared --disable-static --with-tests
python3 waf build -j2
LD_LIBRARY_PATH=$PWD/build ./build/unit-tests --log_level=message
```

Result: **77/77 tests passed**.

Focused fail-first coverage that now passes includes:

- runtime treatment and thread ownership;
- maximum active signer observation;
- queue-full fallback visibility;
- deterministic stale drop;
- serial and worker failure terminal accounting;
- pending-work cancellation on shutdown;
- worker join before `SVSPubSub` state destruction.

### Built-Binary Contract

```text
SPEC137_BENCH_BINARY=build/spec137/bin/svs-serial-production-offload \
  python3 tests/python/test_spec137_svs_serial_production_offload.py -v
```

Result: **5/5 tests passed**. The two self-test runtime expansions differ
exactly in the seven registered treatment fields and no others.

`ldd` resolved the benchmark to the isolated Spec 137 `libndn-svs` and only
Boost 1.71 libraries.

### ASan/UBSan/LeakSanitizer

The GCC 9 `-O2` and `-Og` sanitizer attempts were stopped because compiling the
single template-heavy `svspubsub.cpp` unit exceeded 11 minutes while remaining
CPU-bound. This was a compiler-cost issue, not a source failure. The independent
sanitizer worktree was then configured with Clang 10:

```text
CXX=/usr/bin/clang++ python3 waf configure \
  --enable-shared --disable-static --with-tests --with-debug \
  --with-sanitizer=address,undefined
python3 waf build -j1
```

With `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`, all four targeted gates
passed with no sanitizer report:

- treatment/thread/signer proof;
- fallback visibility;
- pending cancellation and counter conservation;
- shutdown join before dependent-state destruction.

Clang sanitizer output is a safety gate only. It is not a performance subject
and does not change the GCC identity required for T003 and later experiments.

## Boundary

T002 proves that the two runtime modes and their accounting are implementable
in one source patch and one binary. It does **not** prove a performance benefit,
an admissible publication rate, MiniNDN topology correctness, or any formal
Spec 137 conclusion. Those claims remain blocked on T003-T006.
