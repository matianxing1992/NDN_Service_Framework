# Core Neutrality Contract

## Ownership

NDNSF Core owns:

- signed Mapping validation and exact-name resolution;
- generic sample/group extent prediction;
- runtime window/lookahead and Mapping/Payload/retry budgets;
- timeout, Nack, exact-name retry, deadline, and late-arrival handling;
- generic source/repair FEC and recovery provenance;
- generic status and trace events.

UAV-APP owns:

- telemetry field selection and compact serialization;
- latest-state application admission;
- acoustic/audio capture source, format, encoding, encryption, and block
  assembly;
- playback, concealment, visualization, and domain interpretation;
- `GetStatus` snapshot/fallback behavior.

Experiment tooling owns:

- deterministic workload schedules;
- network fault injection;
- formal cell identity and one-shot enforcement;
- metric joining, aggregation, intervals, and verdicts.

## Prohibited Core selectors

No Core or binding decision may be selected by:

- provider/application identity that denotes a UAV;
- telemetry field names or sample meaning;
- `audio`, `acoustic`, microphone, PCM, Opus, or codec identity;
- workload/profile/repetition/cell name;
- experiment result or acceptance threshold.

Generic opaque class IDs may be transported and used only for bounded
same-class extent history. Core must not interpret their text.

## Allowed generic selectors

- stream/session/Mapping version and frontiers;
- sample/group/source/repair kind, index, and bounded extent;
- exact Data name and signer/provider binding;
- production cadence and authenticated observations;
- RTT, backlog, in-flight counts, timeout, Nack, congestion, and retry state;
- recovery capacity, budget, and verified repair bytes;
- configured generic resource limits.

## Audit procedure

Before implementation:

1. use CodeGraph to inventory publisher, consumer, fetcher, Mapping, retry,
   recovery, status, binding, and UAV call paths;
2. record the anticipated changed-file set and ownership;
3. issue PASS/BLOCK.

Before formal execution and at closure:

1. inspect every changed Core/binding symbol and its callers;
2. run a scoped case-insensitive source scan for UAV/telemetry/audio/acoustic/
   codec/workload/cell selector literals;
3. classify each occurrence as APP/experiment/documentation or violation;
4. run deterministic negative tests proving opaque class text does not change
   Core decisions when all generic fields are equal;
5. verify no hidden environment variable or config key selects application
   policy;
6. record zero unresolved violations.

Any violation is a BLOCK. Renaming an application-specific branch with generic
words does not satisfy this contract.

## Core-change rule

The default implementation changes no Core scheduling/recovery behavior.
A Core change is admissible only before formal freeze when:

- a deterministic, application-independent contract defect is reproduced;
- the simplest generic fix is documented;
- tests demonstrate the same behavior with neutral class names and payloads;
- security/migration/rollback effects are audited;
- the pre-implementation audit is renewed.

A formal negative result cannot authorize an in-Spec Core fix or rerun.
