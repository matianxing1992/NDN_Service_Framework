# Research: Historical NDN-SVS Threading Contract

## Material Passport

- **Origin**: Academic Research Suite, `experiment-agent`
- **Mode**: experiment plan and reproducibility audit
- **Date**: 2026-07-22
- **Verification**: SOURCE-VERIFIED DESIGN; CORRECTED QUALIFICATION NOT YET EXECUTED
- **Local inputs**: exact NDN-SVS Git objects, README, public header, examples,
  unit tests, parallel-feature commits, and Spec 133/134 runtime evidence

## Verified Findings

1. `README.md` documents installation and chat execution but contains no
   thread-safety or io_context-ownership statement.
2. `SVSPubSub` public documentation describes the API but contains no
   thread-safety guarantee or allowed-caller-thread statement.
3. `examples/chat-pubsub.cpp` and its regex variant create a Face event thread,
   invoke publication from the application thread, and explicitly claim the
   instance is thread-safe.
4. The claim dates back to the historical example lineage; it was not added by
   the recent async/parallel work.
5. Historical PubSub unit tests call `publish()` using `DummyClientFace` on one
   test thread. They do not stress concurrent Face callbacks and publication.
6. The source contains partial mutexes around version-vector, recorded-vector,
   scheduler call sites, and extra-data state, but no single ownership policy
   covering Scheduler internals, MappingProvider, MemoryDataStore, and every
   notification container.
7. Spec 134's ASan/TSan runs confirmed conflicts on the cross-thread harness.
8. Later parallel commits explicitly separate worker preparation from
   Face/io_context mutation/commit and post results back to that context.

## Interpretation

The evidence does not support either extreme statement:

- “README/API guarantee thread safety” is false because they are silent.
- “The example never used cross-thread publication” is false because it does.

The defensible conclusion is that the example expresses an intended
cross-thread use, but the contract, tests, and implementation do not
consistently uphold it. A performance experiment must not depend on that
contradiction. The narrow single-I/O-thread model avoids the disputed behavior
while measuring the pre-async serial architecture.

## Corrected Experimental Variables

- **Independent variable**: target publication rate in Spec 133; Spec 134 only
  qualifies the execution model.
- **Dependent variables**: attempted rate, missed release slots, remote
  delivery, invalid/error counts, exits, and later per-stage durations.
- **Controls**: exact commit/build patch, two-node topology, payload, security,
  compression, absolute timer semantics, warmup/measure/drain.
- **Confounders controlled**: cross-thread races, catch-up backlog, local
  self-delivery, logging perturbation, accidental repaired-library linkage.

## Timer Choice

Use one independent application `steady_timer` bound to the same
Face/io_context. This does not change NDN-SVS protocol timers. The callback
publishes at most one item and advances past elapsed deadlines before rearming.
It therefore measures the amount of publication work the serial event loop can
actually admit, rather than queueing an artificial offered load.

## Evidence Rules

- Preserve the cross-thread crashes and sanitizer reports as measured but
  ineligible evidence.
- Do not merge or load the experimental repair commit.
- Do not retry qualification automatically.
- Do not call the qualification a throughput experiment.
- Do not resume Spec 133 formal cells without a valid qualification receipt.

## Statistical Posture

Spec 134 has one correctness qualification and makes no population inference.
Spec 133 will have one formal observation per rate and may report descriptive
operation distributions, but no run-level confidence interval or causal claim
about async/multithreading.
