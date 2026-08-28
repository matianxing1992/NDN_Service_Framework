# Feature Specification: Stream Prefetch Retention Recovery

**Feature Branch**: `Experimental`

**Created**: 2026-07-19

**Status**: In Progress

**Input**: Repair NDNSF Streaming/prefetching and verify the repaired path with the UAV application.

## User Scenarios & Testing

### User Story 1 - Continuous Live Consumption (Priority: P1)

As a live-stream consumer, I continue receiving the current stream when short processing or network backlogs approach or exceed the Provider's retained-data window; the session must not wait tens of seconds on Data that can no longer be returned.

**Why this priority**: The current UAV run stops after about 25 seconds even though the Provider continues publishing, making the live API unusable.

**Independent Test**: A deterministic Core test forces retention eviction and proves that evicted Data is not classified as future Data, while bounded timeout/recovery allows later cursors to continue.

**Acceptance Scenarios**:

1. **Given** a mapped name was produced and later evicted, **When** its Interest reaches the Provider, **Then** it is not admitted into the future-Interest table.
2. **Given** future Data is legitimately reserved but not produced, **When** its Interest arrives before production, **Then** it remains bounded and is satisfied after the signed Data is materialized.
3. **Given** the adaptive fetcher reduces its window or Interest lifetime, **When** the consumer schedules work, **Then** the emitted Interest set follows that current decision rather than the original aggregate maximum.

---

### User Story 2 - Low-Lag UAV Video (Priority: P2)

As a UAV operator, I see continuously updating video instead of an increasingly old queue or a frozen final frame.

**Why this priority**: The exact-identity run measured capture-to-decode p95 of 1,052 ms and delivered only 725 of 1,830 produced frames.

**Independent Test**: A fresh 60-second zero-loss MiniNDN run uses the repaired Core through UAV-APP, renders decoded frames throughout the final ten seconds, and reports bounded frame age, Interest work, pending state, and Provider/consumer frontiers.

**Acceptance Scenarios**:

1. **Given** 30 FPS UAV video under zero configured loss, **When** it runs for 60 measured seconds, **Then** decoded and displayed frames continue during the final ten seconds without a retention-induced stall.
2. **Given** a temporary backlog, **When** obsolete live data cannot be recovered before its usefulness deadline, **Then** the application records the drop and resumes from useful current data rather than blocking indefinitely.

### Edge Cases

- A name is reserved but never materialized.
- A name was materialized but its signed Data has been evicted locally.
- Future Interests expire immediately before publication.
- Mapping remains available after payload retention has advanced.
- One incomplete video access unit is followed by later complete units.
- The consumer is slower than the offered encoded packet rate for longer than the retention horizon.

## Requirements

### Functional Requirements

- **FR-001**: The Provider MUST distinguish never-produced reserved names from previously produced but no-longer-retained names.
- **FR-002**: Previously produced but evicted names MUST NOT consume future-Interest capacity or count as eligible future prefetches.
- **FR-003**: The consumer MUST apply the adaptive fetch decision's current aggregate bound, payload budget, mapped cursor range, and Interest lifetime.
- **FR-004**: Future-Interest timing MUST be based on sample-normalized packet demand and current observations, not an immutable session-start production frontier multiplied once per segment.
- **FR-004a**: In the future-on live policy, expiry of the decision-time usefulness deadline MUST terminally skip that cursor and advance bounded recovery; it MUST NOT perform three long retries for obsolete live Data. Recording/beginning playback retains its established retry behavior.
- **FR-004b**: The consumer MUST maintain the payload Interest pipeline independently of Mapping validation and application item processing: receiving one Data frees one network slot and triggers replacement Interest expression before validation/decryption/decoding callbacks may consume that Data.
- **FR-004c**: Network demand MUST use Data Retrieval Delay measured from exact payload Interest expression to matching Data reception. Application capture, encode, encryption, validation, decode, and GUI time MUST NOT be treated as network RTT.
- **FR-004d**: Chasing MUST issue a bounded burst over already mapped exact names; live-edge detection MUST compare normalized sample inter-arrival time with the advertised sample period; steady fetching MUST maintain the resulting bounded in-flight demand. These phases MUST change actual Interest issuance, not status fields alone.
- **FR-005**: All pending Interest, retained Data, Mapping, reorder, and video-frame assembly state MUST remain bounded.
- **FR-006**: UAV-APP MUST provision a bounded retention duration appropriate to its configured packet rate and MUST expose enough status to identify retention lag, stale skips, and future hits.
- **FR-006a**: UAV-APP MUST size the ahead Mapping horizon from the per-Data production period. Packetization remains an APP capacity choice and MUST NOT be used as evidence that the Core prefetch algorithm is correct.
- **FR-007**: UAV-APP's ordered single-access-unit assembly MUST remain bounded, MUST explicitly record an incomplete-unit drop when a later frame begins, and MUST allow that later complete unit to proceed.
- **FR-008**: The repair MUST preserve exact semantic Data names, Provider signature validation, Mapping validation, AES-GCM handling, replay rejection, and optional FEC.
- **FR-009**: Frozen Spec 121/122 result directories MUST remain unchanged; new validation uses a unique Spec 123 result identity.
- **FR-010**: The established `legacy-pipe` rollback remains available until a corrected exact-identity candidate passes the new live-continuity gate.

### Key Entities

- **Payload lifecycle**: reserved, materialized-and-retained, or materialized-and-evicted state for one immutable semantic Data name.
- **Adaptive fetch decision**: the authoritative bounded decision for the next mapping and payload Interests.
- **Live lag horizon**: the distance between useful production and consumer completion relative to retained data.
- **Video access-unit assembly**: one bounded ordered state for exact segments belonging to the current captured frame.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Core regression tests reproduce the old evicted-as-future behavior before the fix and pass after the fix.
- **SC-002**: A 60-second zero-loss UAV MiniNDN run has decoded/displayed activity in every five-second bucket, including the final ten seconds.
- **SC-003**: The run ends with no permanently occupied future-Interest table and no unexplained delivery plateau longer than two seconds.
- **SC-004**: Exactly attributable capture-to-decode p95 is at most 250 ms and p99 is at most 500 ms; if unmet, the result remains a failed performance gate rather than a completion claim.
- **SC-005**: At least 99% of eligible future Interests are satisfied, and Mapping-plus-payload Interest work remains no more than 15% above the configured data-plus-parity items per decoded frame.
- **SC-006**: With the original 3600-byte payload and 12-data-plus-1-parity 8 Mbps configuration restored, a new 60-second UAV run MUST meet SC-002 through SC-005. A run that passes only after reducing packet count does not close the Core prefetch claim.
- **SC-007**: Security, malformed-input, FEC, restart, and current Stream unit regressions pass without weakening validation.

## Assumptions

- MiniNDN is the authoritative network validation environment for this repair.
- The primary defect is correctness under retention pressure; packet-count optimization is included only where required to meet the bounded live gate.
- A single fresh 60-second run closes functional continuity; broader paired performance claims require a later frozen matrix.
