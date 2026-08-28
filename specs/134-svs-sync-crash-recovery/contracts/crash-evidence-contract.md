# Threading And Qualification Evidence Contract

## Evidence classes

### Preserved cross-thread diagnostics

Existing ASan/UBSan and TSan attempts prove that the historical example-style
cross-thread harness reaches concrete races. They retain their original
commands, hashes, logs, and receipts. They are classified:

```text
measured: cross-thread documentation/test/implementation mismatch
not measured: normal single-I/O-thread failure or performance
```

The experimental repair worktree/commit is retained for forensics but is not an
authorized runtime subject.

### Corrected qualification

Every qualification attempt has a new output path, exact command/environment,
source/binary/library hashes, linkage, stdout/stderr, process exits, event
records, corruption scan, and terminal receipt. Existing paths are rejected and
automatic retry is forbidden.

## Execution contract

The topology contains `peer-a` and `peer-b` as two MiniNDN nodes. Each node
starts exactly one application process. After initialization, each process:

1. owns one Face and one SVSPubSub;
2. runs one Face/io_context execution thread;
3. executes the application release timer and synchronous `publish()` on that
   thread;
4. receives Sync/Mapping/payload/subscription callbacks on that thread; and
5. has no second NDN-SVS/Face caller.

Forbidden: `publishAsync`, publisher/pacer/worker threads, Face event thread
plus direct main-thread publication, cross-thread post adapters, and catch-up
queues.

## Absolute release rule

Let `d(i) = start + i * period`. At a timer callback:

1. record `now - d(i)` as lateness;
2. publish at most one item;
3. after publication, find the first `j > i` for which `d(j) > now`;
4. add `j - i - 1` to `missedReleaseSlots`;
5. arm only `d(j)`.

Thus `scheduledSlots = attempted + missedReleaseSlots` within the measured
window, modulo explicitly reported boundary censoring.

## Delivery rule

Payload identity contains cell, sender, logical ID, and measured-phase marker.
The subscription callback classifies:

- own sender: `localDeliveryIgnored`;
- expected remote sender with valid payload: `remoteDelivered`;
- expected remote sender with malformed payload: `invalidRemote`;
- unknown sender: `invalidRemote`.

Local self-delivery never increments remote delivery or invalid counts.

## Terminal receipt

The receipt includes per peer/direction:

- scheduled slots;
- attempted publications and attempted pps;
- missed release slots and maximum/p50/p95 lateness;
- local deliveries ignored;
- valid remote deliveries and delivered pps;
- delivery/attempted ratio;
- invalid remote payloads;
- publication errors;
- process exit and complete event flush;
- exact subject/driver/library hashes and Boost 1.71 linkage;
- corruption-signature findings;
- terminal verdict.

`QUALIFIED` requires two zero exits, complete receipts, nonzero remote delivery
in both directions, zero invalid remote payloads, zero publication errors, and
zero corruption findings. Target-rate attainment is descriptive, not a
correctness gate.

## Handoff

Only `QUALIFIED` admits a new Spec 133 single-I/O-thread preflight. Diagnostic
and qualification outputs never enter the five formal rate tables.
