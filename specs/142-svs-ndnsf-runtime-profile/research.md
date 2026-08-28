# Research: Effective NDNSF NDN-SVS Runtime Profile

## Decision 1: Use protocol V3

**Decision**: Set and verify NDN-SVS protocol V3 in both modes.

**Rationale**: `ServiceUser.cpp` and `ServiceProvider.cpp` default
`NDNSF_SVS_PROTOCOL_VERSION` to V3. Spec 140/141 forced V2 and therefore did
not represent the current NDNSF default.

**Alternative rejected**: Reuse the Spec 140/141 V2 binary. That would preserve
the already identified configuration error and cannot answer the corrected
question.

## Decision 2: Use the effective 800-byte piggyback limit

**Decision**: Resolve `maxPiggyDataSize` from the actual `SVSPubSubOptions`
passed by the current application. The expected current value is the NDN-SVS
default of 800 bytes.

**Rationale**: Current ServiceUser/ServiceProvider code constructs default
`SVSPubSubOptions` and does not consume
`NDNSF_SVS_MAX_PIGGYDATA_BYTES`. Although the MiniNDN performance runner
exports that variable with value 4096, it does not change the effective runtime
option. Runtime-resolved values, not intended environment values, are the
experiment authority.

**Alternative rejected**: Force 4096 in the microbenchmark and call it the
NDNSF profile. That would model runner intent rather than current runtime
behavior and would require a separately reviewed NDNSF code change.

## Decision 3: Measure piggyback eligibility instead of assuming it

**Decision**: Record the fully encoded signed publication wire size,
piggyback-eligible decisions, piggyback hits, and fallback Fetches.

**Rationale**: A 256-byte application payload acquires names, TLV framing, and
an RSA signature. The final Data size, rather than payload size, determines
whether the 800-byte threshold allows piggybacking.

**Alternative rejected**: Infer eligibility from payload size. This could hide
a real Fetch path and recreate the original confound.

## Decision 4: Preserve retry constants but reject retry-active comparisons

**Decision**: Keep current publication and Mapping Fetch retry/lifetime/backoff
values identical in both modes. In the zero configured loss experiment, any
observed retry activation, timeout, or Nack marks the cell
`PROFILE_INVALID` for a clean worker comparison. The frozen constants are:
Mapping window 10, retries 0, and 200 ms failure backoff; publication retries
2, inner retries 2, 500 ms Interest lifetime, 250--2000 ms adaptive lifetime
bounds, 50 ms failure backoff, and 2000 ms maximum backoff.

**Rationale**: Retry settings are part of the deployed runtime but are not the
independent variable. A valid zero-loss worker experiment should not derive its
result from recovery behavior. Overload-induced failures remain valuable
diagnostics and are preserved, but they answer a different question.

**Alternative rejected**: Disable retries only in the benchmark. That would no
longer match NDNSF. Also rejected: accept retry-active cells as equivalent,
because different recovery volume can dominate delivery and latency.

Measurement-window counters are evaluated as `measureEnd - measureStart`.
Warmup and drain activity is preserved but cannot invalidate or sanitize the
formal 60-second window.

## Decision 4a: Preserve V3 extensions across the worker-to-Face handoff

**Finding**: A source and MiniNDN audit found that parallel Sync production
called the extra-block provider on a worker but dropped the returned blocks
when V3 envelope signing was deferred to the Face thread. This produced
`piggybackSent > 0` with `piggybackReceived = 0`.

**Decision**: Preserve the worker-built extension vector in the production
result and encode that exact vector on the Face thread. Add a focused V3 test
for `parallelSyncProductionSigning=false` and
`parallelSyncProductionExtraBlock=true`.

**Evidence**: The focused test passes, the full NDN-SVS suite passes 75/75, and
the canonical r4 campaign observes nonzero piggyback reception. Remaining
measurement-window Fetch timeout/retry events are therefore a separate
boundary, not the discarded-extension defect.

## Decision 5: Match current parallel Sync settings in both modes

**Decision**: Enable the current NDNSF-default parallel Sync processing and
parallel Sync production settings identically in both modes: four workers and
queue 256 in each pool, Sync signing on the Face thread, extra-block preparation
in workers, and batching disabled.

**Rationale**: The requested comparison is publication preparation inline
versus one worker under the existing NDNSF environment. Disabling unrelated
production settings would create another artificial profile. The four-CPU
affinity and all active pools are recorded so resource oversubscription is
visible.

**Alternative rejected**: Disable all other workers to produce a cleaner but
non-NDNSF configuration.

## Decision 6: Retain the symmetric two-peer workload

**Decision**: Both MiniNDN peers continuously publish and subscribe.

**Rationale**: The user explicitly permits retaining this workload. It gives
both nodes simultaneous publication and receive work and isolates NDN-SVS
behavior without NDNSF request-selection application logic.

**Claim boundary**: This does not reproduce the message expansion or role
asymmetry of REQUEST/ACK/SELECTION/RESPONSE. Conclusions are limited to the
NDN-SVS worker mechanism under an NDNSF-effective transport profile.

## Decision 7: Stage the campaign to avoid wasted formal cells

**Decision**: First run a fresh 400 pps matched pair. Authorize the 600/800
cells only after both qualification receipts pass strict identity, profile,
workload, security, Fetch-health, and accounting checks.

**Rationale**: Spec 140/141 already demonstrated the cost of learning about a
configuration error after a full matrix. A two-cell gate is the smallest
evidence that the corrected setup is fit to proceed.

**Alternative rejected**: Launch all six cells before inspecting the resolved
configuration and counters.
