# Feature Specification: NDNSF External Bug Report Corrections

**Feature Branch**: `Experimental`

**Created**: 2026-07-15

**Status**: Draft

**Input**: Correct only the five defects reported by Peter for the older
NDN_Service_Framework/ndn-svs/Python/NAC-ABE stack: failed and degrading
segmented responses, oversized-response Provider abort, unusable Python
Targeted requests, Targeted timeout not firing, and OpenABE exit crashes.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Segmented Responses Do Not Fail or Poison the Provider (Priority: P1)

An application returns a response larger than one Data packet. The requester
receives the exact bytes, the Provider stays alive, and subsequent small
responses continue to work in the same Provider process.

**Why this priority**: This combines email defects 1 and 2, which have the same
reported signed-inner/signed-outer segmentation path and currently cause both
silent loss and process abort.

**Independent Test**: Force the current NDNSF response through the SVS
segmentation path, exercise 6.5-KB, 8-KB, and 16-KB responses, then send 80
consecutive 8-KB responses followed without Provider restart by 10 64-B and 12
4-KB responses.

**Acceptance Scenarios**:

1. **Given** a response that requires SVS segmentation, **When** it is published
   through normal or Targeted invocation, **Then** the requester receives the
   byte-exact response without silent loss or Provider termination.
2. **Given** an inner segment whose outer encapsulation would exceed the packet
   limit, **When** the response is prepared, **Then** the content is split safely
   and no oversized packet is emitted.
3. **Given** a Provider has served an 80-response segmented burst, **When** small
   requests follow in the same process and node-id epoch, **Then** all 10 64-B
   and all 12 4-KB responses complete without a restart.
4. **Given** signing, encoding, validation, storage, or emission fails, **When**
   the asynchronous handler observes the failure, **Then** the failure is
   contained, inconsistent sequence state is not advertised, and the Provider
   remains usable.

---

### User Story 2 - Python Targeted Invocation Works With Current Security (Priority: P1)

A Python Provider registers one service that can be invoked normally and by a
known Provider identity. Both paths use the current mandatory token and replay
protection instead of requiring the reporter's tokens-off workaround.

**Why this priority**: Email defect 3 made Targeted unusable in the reported
wrapper. The current tree appears to contain the intended registration and
tokens-on behavior, but this must be verified on the real binding rather than
assumed from source inspection.

**Independent Test**: Register one real Python service, complete normal and
Targeted calls, then verify missing, mismatched, and replayed tokens fail closed.

**Acceptance Scenarios**:

1. **Given** a Python service registration, **When** the same handler receives a
   normal request and a Targeted request, **Then** both calls complete and the
   intended handler runs exactly once per request.
2. **Given** a valid Targeted bootstrap/token pair, **When** the fast path is
   used, **Then** current authorization, one-time token, and replay checks remain
   active.
3. **Given** a missing, mismatched, or replayed token, **When** a Targeted call is
   attempted, **Then** it fails closed; Spec 112 does not add a public tokens-off
   switch.

---

### User Story 3 - Every Targeted Request Honors timeout_ms (Priority: P1)

An application supplies `timeout_ms` to a Targeted call. The timeout handler
fires exactly once within the declared bound even when the Provider has become
degraded, disappears before publication, or never returns a response.

**Why this priority**: Email defect 4 can leave the application blocked
indefinitely and makes Targeted unsuitable for control or inference workloads.

**Independent Test**: Establish Targeted state, stop or suppress the Provider,
submit another request, and verify one timeout callback no later than
`timeout_ms + 500 ms`.

**Acceptance Scenarios**:

1. **Given** an unavailable Provider, **When** a Targeted request is accepted,
   **Then** its deadline starts immediately and its timeout callback runs once.
2. **Given** a timeout races with a late response, **When** either reaches the
   terminal transition first, **Then** the other cannot invoke a second callback.
3. **Given** request publication throws or does not complete, **When** the caller
   deadline expires, **Then** the same timeout handler still runs and all request
   state is reclaimed.

---

### User Story 4 - NAC-ABE Processes Exit Cleanly (Priority: P1)

Controller, Provider, and User processes that initialize and use NAC-ABE exit
without a crash in OpenABE/RELIC teardown.

**Why this priority**: Email defect 5 affects every process exit and can obscure
test results or corrupt orderly shutdown.

**Independent Test**: Run 100 initialized lifecycle cycles; every cycle starts,
uses, and stops one Controller, one Provider, and one User while recording each
process exit code and signal (300 role exits total).

**Acceptance Scenarios**:

1. **Given** a process has used NAC-ABE, **When** it exits normally, **Then** it
   returns a normal exit code without SIGSEGV or SIGABRT.
2. **Given** a controlled shutdown while callbacks have completed, **When**
   process teardown runs, **Then** OpenABE shutdown is not invoked from an unsafe
   static-destruction order.
3. **Given** the current process-lifetime workaround already satisfies the test,
   **When** the evidence is reviewed, **Then** no speculative teardown rewrite is
   made.

### Edge Cases

- Final outer Data size is exactly at, one byte below, and one byte above 8800 B.
- Producer, service, request, and certificate names increase outer packet size.
- The first, middle, or final segment fails during signing, validation, storage,
  or emission.
- A payload is an exact multiple of the safe segment content budget and must not
  create an unnecessary empty trailing segment.
- Synchronous and asynchronous SVS publication modes exercise the same limits.
- A late response arrives after Targeted timeout.
- Provider shutdown begins with an outstanding segmented publication.
- Current code already fixes an older-wrapper symptom; verification must not
  replace secure behavior with the reporter's historical workaround.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Segmentation MUST use the final signed and encapsulated packet size,
  not a fixed raw-content size, to keep every emitted packet within the active
  8800-B limit.
- **FR-002**: A logical publication MUST NOT advertise sequence state before all
  packets needed for that publication are safely prepared and readable.
- **FR-003**: Encoding, signing, validation, storage, and Face emission failures
  MUST be contained at the asynchronous boundary and MUST NOT terminate or
  permanently degrade the Provider.
- **FR-004**: Segmented normal and Targeted responses MUST reassemble to the exact
  application bytes for both synchronous and asynchronous SVS publication.
- **FR-005**: After 80 consecutive 8-KB segmented responses, the same Provider
  epoch MUST complete 10/10 64-B and 12/12 4-KB responses.
- **FR-006**: Failed or timed-out segmented receive state MUST be reclaimed and
  MUST NOT block later publications or requests.
- **FR-007**: The current automatic large-response reference behavior MUST remain
  unchanged; it MAY be disabled only by the existing test-only environment flag
  to reach the reported SVS path during Spec 112 validation.
- **FR-008**: The real Python binding MUST register the intended handler for both
  normal and Targeted invocation.
- **FR-009**: Python Provider and User bindings MUST keep production tokens
  enabled and MUST NOT expose a public tokens-off bypass.
- **FR-010**: Missing, mismatched, consumed, and replayed Targeted tokens MUST
  continue to fail closed.
- **FR-011**: A Targeted request deadline MUST start when the public API accepts
  and records the request, before admission, bootstrap, or publication can block.
- **FR-012**: Every Targeted request MUST invoke exactly one response or timeout
  callback; a degraded or absent Provider MUST NOT cause indefinite waiting.
- **FR-013**: Late or duplicate events MUST NOT invoke a second terminal callback
  or revive reclaimed request state.
- **FR-014**: Controller, Provider, and User processes that initialize and use
  NAC-ABE/OpenABE MUST exit repeatedly without SIGSEGV or SIGABRT.
- **FR-015**: Spec 112 MUST preserve Controller permissions, NAC-ABE routing,
  Provider permissions, one-time tokens, and replay protection.
- **FR-016**: Current ndn-svs and its relevant tests MUST build against the
  project's declared local Boost 1.71 baseline; a stale test binary is not
  admissible evidence.
- **FR-017**: MiniNDN validation MUST bind results to exact source, dirty diff,
  dependency, binary, configuration, and script identities and retain negative
  results without overwriting them.
- **FR-018**: Each declared pre-fix or final candidate/cell pair MUST run once;
  any source or configuration change creates a new candidate.
- **FR-019**: Final network acceptance MUST use MiniNDN at 0% configured loss and
  MUST force the reported inline-SVS path explicitly in the isolated test roles.
- **FR-020**: Changes MUST be confined to the five reported defects, their build
  prerequisite, tests, evidence, and documentation.

### Key Entities

- **Segmented Publication**: One logical response, its final inner/outer packet
  sizes, sequence identity, prepared/advertised state, and failure record.
- **Targeted Request Deadline**: Acceptance time, absolute deadline, winning
  response-or-timeout state, callback count, and late-event count.
- **Provider Health Epoch**: One Provider process/node-id epoch containing the
  burst and its post-burst small-response checks.
- **Process Exit Record**: Role, NAC-ABE use, shutdown cause, exit code, signal,
  and optional sanitizer result.
- **Evidence Candidate/Cell**: Immutable source/binary/configuration identity and
  one declared MiniNDN test cell with a unique result directory.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: At 0% configured MiniNDN loss with automatic large-response
  externalization disabled for the test roles, 64-B, 4-KB, 5-KB, 6.5-KB, 8-KB,
  and 16-KB responses complete byte-exactly for Normal/Targeted ×
  synchronous/asynchronous SVS publication.
- **SC-002**: One same-epoch 80×8-KB burst is followed without Provider restart
  by 10/10 successful 64-B and 12/12 successful 4-KB responses.
- **SC-003**: Unit and MiniNDN acceptance evidence contains zero packets above
  8800 B, zero Provider SIGABRT/SIGSEGV events, and zero advertised but
  unreadable publications.
- **SC-004**: Real Python normal and Targeted invocations both complete with
  tokens enabled, while missing/mismatched/replayed-token cases all fail closed.
- **SC-005**: Every degraded/absent-Provider Targeted test invokes exactly one
  timeout callback no later than `timeout_ms + 500 ms`; late responses invoke no
  additional callback.
- **SC-006**: 100 initialized lifecycle cycles (100 Controller, 100 Provider,
  and 100 User exits; 300 role exits total) finish with no SIGSEGV or SIGABRT.
- **SC-007**: Current ndn-svs unit tests are rebuilt from the recorded source and
  pass on the Boost 1.71 local baseline before final MiniNDN acceptance.
- **SC-008**: Every pre-fix and final cell has one candidate identity, owner,
  immutable result directory, command/configuration record, and retained outcome.
- **SC-009**: The final audit finds no task, API, transport profile, security
  mechanism, or experiment whose purpose is outside the five email defects.

## Assumptions

- Email defects 1 and 2 share the same ndn-svs segmentation/outer-encoding root
  until tests falsify that hypothesis.
- The reporter's cited commits describe an older stack. Current code is changed
  only when current, rebuilt tests expose the defect.
- The existing automatic large-response reference path is not redesigned,
  hardened, or accepted as a substitute for the forced SVS reproduction path.
- MiniNDN is the only network validation environment for Spec 112; real Wi-Fi,
  Docker, iTiger, UAV, DI, throughput tuning, and 5% loss experiments are out of
  scope.

## Explicitly Out of Scope

- New public checked-publish APIs or changes to existing publication signatures.
- New requester failure/status protocols or additional wire namespaces.
- Large-object reference redesign, exact-name authentication redesign, or new
  inline-versus-reference policy.
- Direct exact-name object benchmarking, 5% loss characterization, general
  performance optimization, Docker/iTiger work, and Spec 111 implementation.
