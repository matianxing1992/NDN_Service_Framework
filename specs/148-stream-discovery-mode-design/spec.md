# Feature Specification: Predictive Stream API Replacement

**Feature Branch**: `[148-stream-discovery-mode-design]`

**Created**: 2026-07-25  
**Status**: Complete — closed by Spec 151 successor evidence

**Input**: Make the `// Predictive:` API the only public high-level stream
publication API. Implement `start()` + `push(signedData)` + `flush()` and remove
the old high-level `start(initial...)` + `announce()` + `publish()` API from
C++, Python, examples, tests, and applications.

## Intent

The current high-level facade exposes two competing workflows:

```text
Old facade:
  start(initial sample) -> announce(sample) -> publish(sample)

Target facade:
  start() -> push(App-signed sequential Data)* -> flush()
```

Spec 148 is a **replacement**, not an additive compatibility feature. After
completion, an application cannot call the old high-level methods. There are no
deprecated aliases and no `start_predictive()` compatibility name.

This removal applies to the public `StreamPublisher` facade and its Python
wrapper. Generic Core internals such as `LiveStreamPublisher`,
`reserveAhead()`, `announceSample()`, `publishSample()`, Mapping/FEC codecs, and
adaptive-fetch state may remain when they are useful implementation primitives.
They MUST NOT be exposed as a second application-level publication workflow.

## Protocol Model

The application gives each source segment its predictable sequential stream
name, creates a complete signed `ndn::Data`, and calls `push()`. Core validates
and retains that exact signed wire; it does not wrap or re-sign the source.
Consumers pre-express Interests for the predictable names. `flush()` commits a
publication group, generates configured repair packets, and advances the
authenticated frontier.

```text
Old public path:
  Mapping Interest -> Mapping Data(name)
  Payload Interest -> Payload Data(content)

Predictive public path:
  Future Interest(sequential name) waits in PIT
  push(exact App-signed Data) -> matching Data
  flush() -> repair packets + authenticated frontier
```

The target removes the extra application-visible Mapping-to-Payload lookup.
Internal Mapping/FEC structures may still encode authenticated segment/group
metadata; they do not create a second Payload Interest.

## API Migration Boundary

- Remove from C++ `StreamPublisher`:
  `start(initialSampleId, initialSampleClass, opaqueItems)`,
  `announce(sampleId, sampleClass)`, and
  `publish(sampleId, opaqueItems)`.
- Remove the corresponding Python methods and exports.
- Expose only `start()`, `push()`, `flush()`, `status()`, and `stop()` for
  high-level publication.
- Replace `start_predictive()` with `start()`; do not retain it as an alias.
- Replace the high-level consumer entry with a
  `PredictiveStreamDescriptor`-based subscription.
- Migrate every in-repository caller in the same implementation change.
- A compile-time/source-scan gate must prove the removed high-level signatures
  no longer exist.

Specs 125, 144, 146, and 147 and their result artifacts are frozen historical
evidence and MUST NOT be edited or rerun. Spec 148 explicitly supersedes Spec
147's public facade contract. A comparison with the old behavior uses a pinned
old binary/commit and preserved command manifest, not a compatibility path in
the new product API.

## User Stories

### US1 — One predictive publication workflow (P1)

An application starts a stream, submits complete App-signed sequential Data
packets with `push()`, and commits each group with `flush()`. There is no
announce/reservation step in the high-level API.

### US2 — Exact App-signed source ownership (P1)

Core routes, buffers, and retransmits the exact source Data wire supplied by
the App. Core signs only Core-owned metadata and repair packets.

### US3 — Predictive subscription and recovery (P1)

A consumer prefetches predictable sequential names, validates every returned
packet, uses repair before bounded retry, and reports explicit terminal gaps.

### US4 — Repository-wide migration (P1)

C++ and Python examples, tests, UAV Drone publication, and Ground Station
subscription use only the replacement API and compile without compatibility
aliases.

### US5 — Real MiniNDN acceptance (P1)

Two real MiniNDN nodes execute the migrated UAV video path. A fresh new-result
run validates behavior; any old/new performance comparison identifies both
immutable binaries and configurations.

## Functional Requirements

- **FR-001**: `PredictiveStreamDescriptor StreamPublisher::start()` MUST be the
  only public high-level start operation and require no bootstrap payload.
- **FR-002**: `push(std::shared_ptr<ndn::Data>)` MUST accept a complete,
  App-named and App-signed source packet, validate its session authority,
  canonical sequential name, epoch, signature/wire budget, and duplicate-name
  state, then retain and satisfy pending Interests with the exact wire.
- **FR-003**: Core MUST NOT wrap, mutate, or re-sign pushed source Data.
  Byte-for-byte wire identity before and after Core storage is mandatory.
- **FR-004**: `flush()` MUST atomically commit the pending group, generate the
  configured generic repair packets, publish an authenticated frontier, and
  clear only the committed pending set. Empty flush is a no-op.
- **FR-005**: Repair metadata MUST identify group membership, source lengths,
  source count, repair count, and session epoch; unequal source lengths MUST
  not be silently truncated.
- **FR-006**: The descriptor/checkpoint MUST contain the stream/session
  authority, sequential naming rule, current retention/frontier state,
  production period, and generic FEC options needed by the consumer.
- **FR-007**: The consumer MUST validate provider trust, expected name,
  session epoch, signed group metadata, wire budget, and duplicate/equivocation
  state before admission.
- **FR-008**: The consumer MUST reuse the existing generic adaptive prefetch
  controller. Spec 148 MUST NOT add UAV, video, audio, codec, or workload
  branches to Core.
- **FR-009**: Recovery order MUST be repair first, then bounded retry when
  required, then explicit terminal skip/gap. Timeout, Nack, retry, recovery,
  and useless-Interest accounting MUST remain observable.
- **FR-010**: `push()` and `flush()` MUST define safe concurrent/stop
  semantics. A stopped stream rejects subsequent mutations.
- **FR-011**: The C++ and Python high-level surfaces MUST be symmetric in
  method set, descriptor fields, defaults, errors, and lifecycle.
- **FR-012**: The old high-level methods and `start_predictive()` MUST be
  removed from headers, implementations, pybind11 exports, Python wrappers,
  examples, and application callers. No compatibility alias is permitted.
- **FR-013**: All repository callers MUST migrate atomically. Failure to build
  any migrated caller blocks completion.
- **FR-014**: Specs 125/144/146/147 source evidence and result artifacts MUST
  remain unchanged. Spec 147's public facade contract is superseded, not
  rewritten.
- **FR-015**: Rollback MUST use the previous pinned binary/commit. Runtime
  rollback to a retained old high-level API is forbidden.
- **FR-016**: UAV Drone and Ground Station MUST both execute the predictive
  path; merely declaring members or parsing an unused mode flag is insufficient.
- **FR-017**: A fresh MiniNDN run MUST prove delivery, exact-wire integrity,
  active future hits, zero separate Payload-layer Interests, recovery behavior,
  and complete latency/traffic metrics.
- **FR-018**: Any old/new latency claim MUST use immutable subject hashes and
  matched topology, workload, security, FEC, loss, and measurement windows.

- **FR-019**: The real UAV acceptance topology MUST launch
  `App_ServiceController` and `UavGroundStationApp` on `memphis`, and
  `UavDroneApp` on `ucla`, with one real NFD per MiniNDN node and the real UAV
  configuration/video source. Fake publishers, Python stream doubles, local
  host NFD, and standalone probe executables are inadmissible substitutes.

- **FR-020**: MiniNDN validation MUST distinguish: (a) launcher/config
  quick-smoke, which is preflight evidence only; (b) a short real UAV
  end-to-end smoke; and (c) a formal run with at least 60 seconds of measured
  traffic after readiness/warm-up. Neither quick-smoke nor syntax/import checks
  support performance or migration-completion claims.

- **FR-021**: The new product binary and runner MUST have no selectable
  Mapping-first public path. `--discovery-mode mapping-first|predictive` and
  `NDNSF_UAV_DISCOVERY_MODE` MUST NOT create a dual-path product interface.
  Historical comparison MUST launch a separately pinned pre-migration binary.

- **FR-022**: Both UAV endpoints MUST emit structured runtime proof containing
  role, mode, session/stream identity, and actual Core object state. Provider
  evidence MUST cover start/push/flush; Consumer evidence MUST cover predictive
  subscription, future-Interest activity, admission, and stop. The analyzer
  MUST fail when a flag is set but the application does not consume it, when
  either endpoint lacks proof, or when an old-path marker appears.

- **FR-023**: Fresh MiniNDN acceptance MUST include one zero-loss cell and one
  fixed light loss/reordering cell. Each cell MUST preserve its exact command,
  topology/config hashes, native/Python/UAV binary hashes, environment,
  per-process logs, NFD/PIT observations, analyzer version, and machine-readable
  summary. The impairment cell validates repair→bounded-retry→terminal-gap
  behavior; it does not authorize algorithm tuning.

## Edge Cases

- Wrong prefix, stale epoch, malformed sequential component, invalid signature,
  oversize wire, duplicate name, or same-name/different-wire: reject/fail
  closed.
- `flush()` with no pending sources: no-op.
- One or a partial final repair group: encode exact source count and lengths.
- Concurrent `push()` racing `flush()`: each accepted source belongs to exactly
  one committed group.
- Stop racing mutation: after stop wins, no Data/frontier is published.
- Late join: fetch and validate frontier, then begin at the advertised live
  position without replaying the entire retained history.

## Success Criteria

- **SC-001 (L2)**: Only the replacement C++/Python high-level API compiles and
  imports; negative compile/source checks reject all removed signatures.
- **SC-002 (L2)**: Exact signed source wire is retained and delivered
  byte-for-byte; Core does not sign source packets.
- **SC-003 (L2)**: Group commit, unequal-length repair, concurrency, stop,
  late-join, equivocation, and recovery tests pass.
- **SC-004 (L2)**: Full native and Python test suites pass after all callers
  migrate.
- **SC-005 (L3)**: UAV Drone and Ground Station complete a real predictive
  smoke with continuous delivery and zero separate Payload Interests.
- **SC-006 (L4)**: Fresh MiniNDN evidence reports delivery, AoI/end-to-end
  mean/p50/p95/p99, longest gap, future-hit, Mapping/Payload Interests, retry,
  timeout, Nack, recovery, and useless-Interest ratio.
- **SC-007 (L4)**: If performance superiority is claimed, the pinned old/new
  comparison satisfies FR-018 and reports negative results unchanged.
- **SC-008 (L3)**: A short real UAV smoke launches the three required
  application processes across `memphis` and `ucla`, proves both endpoint
  runtime markers, decodes video, and exits successfully.
- **SC-009 (L4)**: Fresh zero-loss and fixed light-loss/reorder 60-second cells
  produce complete manifests and all metrics required by FR-017/FR-023.

## Out of Scope

- Changing the existing adaptive prefetch control law.
- Introducing a new workload-specific FEC algorithm.
- Editing or selectively rerunning frozen Spec evidence.
- Keeping the removed high-level API for compatibility.
