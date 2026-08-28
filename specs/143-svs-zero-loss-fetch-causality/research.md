# Research: Zero-Loss Fetch Timeout Causality

## Decision 1: Diagnose before changing recovery

**Decision**: Add observability and run one diagnostic cell; do not change
retry, lifetime, window, piggyback, protocol timing, or scheduling.

**Rationale**: Spec 142 has aggregate evidence of timeout/retry activation but
not the causal branch. Tuning before attribution could suppress the symptom and
destroy the evidence needed to fix the correct layer.

## Decision 2: Instrument three ownership boundaries

**Decision**:

- `Fetcher`: queue, dispatch, Data, Nack, timeout, validation result;
- `SVSyncBase`: producer Interest receipt, DataStore hit/miss, Data put;
- `MappingProvider`: producer Mapping query, empty response set, Data put;
- `SVSPubSub`: Mapping/publication semantic context and outer retry.

**Rationale**: Fetcher owns an Interest attempt but cannot know whether it is a
Mapping or publication fallback. SVSyncBase owns the publication store lookup.
MappingProvider owns its separate query/response path. SVSPubSub owns semantic
retry state. A single layer cannot supply the complete causal chain.

**Alternative rejected**: Infer the cause from aggregate counters. A timeout
with zero Nack is compatible with multiple mechanisms.

## Decision 3: Use structured NDN_LOG, not a new public API

**Decision**: Emit parseable key-value `NDN_LOG_TRACE` lines under dedicated
components and enable only those components in the diagnostic runner.

**Rationale**: This is diagnostic observability, not stable application
behavior. A callback or public trace API would expand the NDN-SVS contract and
introduce additional concurrency/ownership questions.

## Decision 4: Correlate attempts without assuming synchronized clocks

**Decision**: Correlate by full Interest name, Interest Nonce observed on both
peers, and attempt ID within the consumer process. Use per-process monotonic
differences for durations. Use wall-clock/log order only as a consistency check
and never report one-way delay from independent monotonic clocks.

**Rationale**: The Nonce disambiguates repeated Interests with the same name.
MiniNDN namespaces share a host kernel clock in this setup, but the analyzer
should not depend on that accidental deployment property.

## Decision 5: Start with the worker cell only

**Decision**: Run worker-rsa at 400 pps first and stop after one cell unless the
recorded conditional criterion authorizes inline.

**Rationale**: Spec 142 observed timeout/retry in worker mode while maintaining
complete delivery. This maximizes the chance of reproducing the boundary with
less unrelated Face backlog and avoids repeating a matched performance matrix
for a causal-path question.

## Decision 6: Separate inner and outer retries

**Decision**: Report `Fetcher::m_retryCount` separately from
`SVSPubSub::m_publicationRetryActivations`.

**Rationale**: Inner retry repeats the same NDN Interest after its lifetime.
Outer retry restarts the publication state machine after an exhausted Fetcher
attempt. Adding them into one number obscures which control loop activated.

## Decision 7: Measure CPU in the new run only

**Decision**: Snapshot `getrusage(RUSAGE_SELF)` at the exact measurement
boundaries and report both one-core and four-core-normalized utilization.

**Rationale**: Spec 142 did not record CPU utilization. It cannot be reconstructed
reliably from retained artifacts. The new resource values are diagnostic
context and are never backfilled into Spec 142.
