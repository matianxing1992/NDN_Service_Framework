# Data Model: Loss and Reordering Resilience

## CursorLifecycle

Fields: session epoch, cursor, semantic name digest, active attempt generation,
attempt count, usefulness deadline, state, terminal reason, ahead-of-frontier
flag, expressed/received timestamps.

States:

```text
Pending -> Received -> Validating -> Delivered
   |          |             |        Recovered
   |          |             |        Skipped
   +-> Pending(next attempt)          TerminalUnproduced
                                     Stopped
```

Rules: terminal states never transition; only the active attempt can transition
network ownership; stop/session generation fences every callback; records are
removed only after terminal history retention permits it.

## RecoveryGroupState

Fields: group ID, class, source cursors/names/lengths, selected repair cursor and
name, received-source bitmap, recovered index, deadline, terminal reason.

Rules: membership comes only from verified Mapping/repair envelopes; one repair
recovers exactly one missing source; application admission happens once;
multiple-source loss cannot produce output.

## ReorderObservation

Fields: session, media sequence, publication cursor, arrival order, emitted
order, lateness, pending depth/bytes, disposition.

Dispositions: buffered, emitted, duplicate, stale-session, late-terminal,
deadline-skipped, overflow-skipped, invalid.

Rules: no payload bytes or secrets are recorded; maximum depth and counts are
bounded aggregate status, while detailed events use the shared sampler.

## ImpairmentCell

Fields: cell ID, loss percentage, delay, jitter/distribution, reorder percentage,
correlation, gap, repetition count, duration, workload constants, expected
command IDs.

Rules: immutable after campaign preflight; every command ID executes at most
once; all endpoints must report the effective profile before admission.

## RunEvidence

Fields: run ID/path, source hash, command/environment, endpoint qdisc before and
after, process ownership, return code, duration, decoded frames/gaps/stalls,
Interest/future-hit counts, retries, recoveries, late/duplicate/deadline events,
latency distribution, verdict and reasons.

Rules: one unique directory per invocation; missing or malformed required data
is a failed measured outcome; run artifacts are immutable after summarization.
