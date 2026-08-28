# Measurement Contract: Pure NDN-SVS PubSub Commit Comparison

## 1. Normative Experiment Identity

```yaml
schemaVersion: spec131-campaign-v1
subjects:
  - id: baseline-sync-serial
    commit: a9944019f76791773604999f00128057b9534ace
    publishApi: publish
    parallelReceive: null
    parallelProduction: null
  - id: latest-async-parallel
    commit: 6bb34545b4f89f1f6c265a68c18f1a40ade413eb
    publishApi: publishAsync
    parallelReceive: {workers: 4, queue: 4096}
    parallelProduction:
      workers: 4
      queue: 4096
      signInWorker: true
      buildExtraBlockInWorker: true
common:
  buildCompatibility:
    hostBoostVersion: "1.71"
    originalMinimumVersion: "1.74"
    patchPath: wscript
    replacements:
      - {from: "BOOST_VERSION_NUMBER < 107400", to: "BOOST_VERSION_NUMBER < 107100"}
      - {from: "minimum supported version of Boost is 1.74.0", to: "minimum supported version of Boost is 1.71.0"}
    identicalPatchForBothSubjects: true
    temporaryLocalBranchesOnly: true
    mergeRebasePushForbidden: true
  wireVersion: v2
  syncInterestLifetimeMs: 1
  suppressionMs: 500
  periodicMs: 30000
  periodicJitter: 0.1
  syncBatching: false
  ratesPps: [200, 400, 600, 800, 1000]
  repetitions: 1
  convergeSeconds: 5
  warmupSeconds: 10
  measuredSeconds: 60
  drainSeconds: 10
  payloadBytes: 256
  topology: {hosts: 2, bandwidthMbps: 100, oneWayDelayMs: 10, lossPercent: 0}
  cpuAffinity: {sharedLogicalCpus: [0, 1, 2, 3], dedicatedPerPeer: false}
automaticRetry: false
```

Any change creates a new campaign schema/version and invalidates comparison
with receipts generated under this contract.

## 2. Boost 1.71 Build Compatibility Contract

Both subjects originate from source whose `wscript` rejects Boost older than
1.74. The evaluation host uses Boost 1.71. Candidate creation MUST therefore:

1. create one temporary local branch/worktree from each exact base commit;
2. verify that the unmodified `wscript` contains both expected 1.74 strings;
3. apply only these two textual substitutions:

   ```diff
   -if conf.env.BOOST_VERSION_NUMBER < 107400:
   +if conf.env.BOOST_VERSION_NUMBER < 107100:
   -    conf.fatal('The minimum supported version of Boost is 1.74.0.\n'
   +    conf.fatal('The minimum supported version of Boost is 1.71.0.\n'
   ```

4. commit that patch locally so the build worktree is clean and its temporary
   head/tree are reproducible;
5. require the canonical patch bytes and SHA-256 to be identical for baseline
   and treatment, while retaining their distinct base/head/tree identities;
6. reject any path or hunk outside `wscript` and reject any additional
   `wscript` change;
7. configure and build both subjects against the same recorded Boost 1.71
   include/library paths and the same compiler/dependency environment;
8. record `ldd`/ELF evidence proving every resolved Boost library is 1.71 and
   no Boost 1.74 object is loaded; and
9. never merge, rebase, push, or use the temporary branch heads to move the
   active NDN-SVS branches. Temporary worktrees/branches may be removed only
   after the campaign closes and their manifests/patches remain preserved.

The benchmark subject identity is
`base commit + canonical Boost-1.71 build-only patch`, not the temporary branch
head alone. A patch mismatch blocks comparison before either formal block.

## 3. PubSub Peer Contract

The C++ peer has two roles and one compile-time subject adapter:

```text
svs-pubsub-bench --role publisher|subscriber
  --subject baseline-sync-serial|latest-async-parallel
  --sync-prefix NAME --node-prefix NAME --peer-prefix NAME
  --rate-pps N --warmup-s 10 --measure-s 60 --drain-s 10
  --payload-bytes 256 --events PATH --summary PATH
```

Publisher obligations:

1. run the Face `io_context` on an independent event-loop thread using
   non-blocking handler dispatch;
2. run a separate high-resolution pacer thread that waits until each absolute
   monotonic release deadline and directly calls a common thread-safe
   publication adapter;
3. implement that adapter with `io_context::post`, count a successful post as
   `attempted`, and execute the subject `publish()`/`publishAsync()` on the Face
   thread because the historical scheduler cancel path is unsafe under direct
   concurrent entry;
4. never use the Face scheduler to generate offered load;
5. pin the pacer to logical CPU 0 and Face thread to logical CPU 1; request
   `SCHED_FIFO` priority 1 for the pacer, use a bounded 50 us final spin, and
   record whether every affinity/priority operation succeeded;
6. write `scheduled`, `api-enter`, `api-return`, and sequence events to an
   in-memory/batched sink;
7. never run an unbounded catch-up loop; skip and count a slot more than two
   periods late, and record every actual wake time;
8. publish the exact payload schema and unique application name;
9. remain alive through the drain interval to serve publication Data.

Subscriber obligations:

1. register subscription before readiness is declared;
2. record the first update range covering each sequence;
3. validate logical ID, payload length/schema/digest;
4. record first and duplicate callbacks without blocking the Face thread;
5. stay alive until drain completion and flush evidence once.

The treatment adapter MUST call:

```cpp
pubsub.getSVSync().getCore().setParallelSyncProcessing(true, 4, 4096);
pubsub.getSVSync().getCore().setParallelSyncProduction(
  true, 4, 4096, true, true);
pubsub.getSVSync().getCore().setSyncInterestBatching(false);
pubsub.publishAsync(...);
```

The baseline adapter MUST compile without references to these unavailable APIs
and call `pubsub.publish(...)`.

## 4. Payload Contract

The payload is exactly 256 bytes and contains fixed-width network-order fields:

```text
magic[8] = "SVS131\0\0"
schemaVersion:u16
phase:u8
reserved:u8
logicalId:u64
scheduledNs:u64
campaignCellHash[16]
deterministic filler to byte 255
```

Filler is generated from `SHA-256(cellHash || logicalId || counter)` and is
verified at the subscriber. The application name includes the logical ID but
the payload is the authoritative timestamp record.

## 5. MiniNDN Cell Contract

- Create a new work directory and topology for every cell.
- Start one NFD in each namespace and wait for its socket.
- Configure multicast for the unique Sync prefix on both NFDs.
- Add explicit neighbor routes for the Sync prefix and the two stable peer
  node prefixes. These are topology routes needed to reach the remote
  `SVSyncBase`/Mapping producer filters.
- Start capture and resource sampling before the subscriber.
- Start subscriber, wait for its machine-readable `READY`, then wait the
  convergence interval and start publisher.
- Terminate only after publisher completion and subscriber drain, or the
  bounded cell deadline.
- Always collect routes, qdisc, NFD status, commands, environment whitelist,
  process exit codes, raw events, and hashes before cleanup.

No NLSR is required for this direct topology. No route to a concrete
application publication name or transient local application face may be
injected to compensate for registration failure.

## 6. Clock Contract

Before formal admission, run a bounded publisher/subscriber namespace probe.
Both namespaces MUST report the same kernel boot ID and identical time-namespace
offset configuration, then sample `CLOCK_MONOTONIC_RAW` around repeated
orchestrator barriers. The samples must remain monotonic and their apparent
offset must remain inside the measured scheduling interval; no fixed
sub-millisecond scheduler-response threshold is assumed. The probe does not
estimate network delay; it verifies the shared timebase assumption. Failure
blocks the formal campaign.

## 7. Pacing and Rate Contract

For rate `r`, deadline `i` is `start + i/r`. The independent pacer records one
`scheduled` event for every release opportunity. A slot more than two periods
late is marked missed and skipped. The publisher never enters a separate loop
to repay accumulated work. `attempted` is the number of adapter admissions.
Attempted rate is attempted publications divided by exactly 60 measured
seconds. A cell is sender-limited when its attempted-rate deviation exceeds
2%. Both 1000 pps admission smokes MUST be non-sender-limited before sealing.

Warmup publications use the same pacer and payload but are excluded from
primary summaries. Drain contains no new scheduled publications.

## 8. Event Contract

Each JSONL event contains:

```json
{
  "schemaVersion": "spec131-event-v1",
  "cellId": "...",
  "role": "publisher|subscriber",
  "event": "scheduled|attempted|api-enter|api-return|state-update|delivery|duplicate|error",
  "logicalId": 1,
  "svsSeqNo": 1,
  "phase": "measured",
  "monotonicRawNs": 0,
  "payloadSha256": "...",
  "details": {}
}
```

Fields not applicable to an event are omitted. Event files are append-only
within a cell and hashed at closure.

## 9. Summary Contract

Percentiles use the nearest-rank method over per-publication observations.
Delivered-only and deadline-capped distributions are separate. Missing items
use their individual drain deadline in the capped distribution. A baseline
worker metric is JSON `null`, not `0`, `"unavailable"`, or absent.

Every cell summary separately reports `scheduledMeasured`,
`attemptedMeasured`, `apiCompletedMeasured`, `deliveredMeasured`,
attempted/scheduled, and delivered/attempted. It MUST NOT use
delivered/scheduled as a substitute for either boundary.

The analyzer rejects:

- duplicate cell IDs or attempts;
- treatment execution before 5 terminal baseline receipts;
- a rate set other than the frozen 10 cells;
- source/binary/manifest hash drift;
- unaccounted emitted items or negative time intervals;
- hidden missing items or mismatched payloads;
- an NDNSF dependency/process/wire marker;
- a summary claiming component-level causality.

## 10. Formal Execution Contract

Preflight and smokes are non-formal. After manifest sealing:

```text
cell attempt count = 1
automatic retry = false
selective replacement = false
baseline block must close before treatment begins
```

If a cell fails, preserve it and mark the campaign `INCOMPLETE` or negative as
appropriate. Any corrected execution uses a new campaign directory and reruns
all 10 cells; prior evidence is never overwritten.

## 11. Claim Contract

Allowed:

> At rate R under the pinned MiniNDN configuration, the latest async/parallel
> version bundle improved/regressed/was inconclusive relative to the pinned
> pre-feature version, with the reported direct delivery and delay observation.

Forbidden:

> Multithreading alone caused X% improvement.

The forbidden claim requires a separate component-isolation experiment.
