# T009 Core and Workload Neutrality Audit

**Date**: 2026-07-24  
**Code-neutrality verdict**: **PASS**  
**Security-contract verdict**: **FAIL (FR-011)**  
**Formal evidence verdict**: **MEASURED NEGATIVE**

## CodeGraph Ownership Trace

CodeGraph traced the formal application bytes through:

```text
makeSources()
  -> CompactTelemetrySample::encode() / OpaqueAcousticSource::encode()
  -> LiveStreamPublisher::publishSample()
  -> publishGroupImpl()
  -> makePayloadPacket()
  -> signed exact-name Data
```

Core sees only `std::vector<uint8_t>` source bytes and generic
source/repair/group metadata. `publishGroupImpl` creates repair symbols from
the declared generic extent, stages every source/repair atomically, and calls
`makePayloadPacket`; it contains no UAV, telemetry, acoustic, codec, workload,
or formal-cell branch.

The executable-selector scan over `Stream.hpp`, `Stream.cpp`, `_ndnsf.cpp`,
and `streaming.py` found zero prohibited decision branches. The 17 textual
matches are documentation/comments, generic `NetworkTelemetrySnapshot`
selection metadata unrelated to UAV sensor payloads, and an existing
key-frame explanatory comment. No behavior is selected by those strings.

The sensor APP contains only its expected `telemetry`/`acoustic` workload
selection and deterministic payload construction. A second scan found no
video class, FPS/GOP, codec, key/delta-frame, or video-threshold behavior in
the sensor helper, node, launcher, matrix runner, or application tests.

## Spec 145 Reference Use

The implementation reused only the corrected reference pattern:

- the APP freezes announcement and actual publication truth;
- Core owns Mapping v2, adaptive sample-atomic scheduling, future Interests,
  bounded retry, and generic recovery;
- the consumer admits complete exact-name samples only after identity,
  session, Mapping, kind/index, and extent checks;
- reported runtime status comes from the actual Core handle.

No Spec 145 result entered the 32-cell denominator. Its files and promoted
result hashes were not changed or rerun.

## Security Finding

### N-001 — FR-011 ciphertext binding is absent

**Severity**: Critical  
**Disposition**: open; deferred because the formal subject is frozen

`makeSources()` returns the encoded telemetry/acoustic bytes directly.
`makePayloadPacket()` copies those bytes into the `LiveStreamSampleEnvelope`
and signs the Data, but performs no encryption. Signing authenticates the
exact Data name and content; it does not provide confidentiality. Therefore:

- exact-name integrity and provider authentication are implemented;
- Mapping and diagnostic logs do not contain the payload bytes;
- the formal Payload Data contains signed plaintext application content;
- the formal matrix is not evidence for encrypted telemetry/audio or
  ciphertext-to-name binding.

This violates FR-011 and the protection wording in T004/T005. The issue was
found after formal freeze, so the subject, analyzer, thresholds, and 32 cells
remain unchanged. A new Spec must add an APP-owned authenticated-encryption
envelope whose associated data binds stream ID, session epoch, exact mapped
Data name, group/item index, and item kind, then run new security and network
evidence.

## Evidence Finding

### N-002 — derived recovery success-rate semantics are invalid

**Severity**: High  
**Disposition**: open; derived field excluded from claims

The frozen analyzer divides `coreRecoveredSources` by `recoveryAttempts`.
One recovery attempt can restore two source items, so this is not a success
probability and can exceed 100% (1,502/1,297 in acoustic-reorder-r01).
The raw attempt, exhaustion, recovered-block, APP recovered-source, and Core
recovered-source counters remain preserved, but `recovery.successRate` is
unavailable as a trustworthy ratio. The field did not control any frozen cell
or treatment pass/fail decision.

A new Spec must define separate denominators for attempt success
(`successfulAttempts / attempts`) and source recovery
(`recoveredMissingSources / recoverableMissingSources`) and must test
conservation between APP and Core provenance.

## Historical Immutability

The T009 read-only recomputation matched T001:

| Frozen root/artifact | SHA-256 | Result |
|---|---|---|
| Spec 127 definition | `6756fba7fad110f267863f4bb9f5cb1abec5c68515642ba383235a6a6729136d` | unchanged |
| Spec 128 definition | `6f532069382d0dc6b3d853c3ab9251e71993e985ae98a1a57c955d719e03faee` | unchanged |
| Spec 127 canonical result | `ef35eb227db611253afe99d312e3eaa18e8c7123898570815cde8756ffd96787` | unchanged |
| Spec 128 canonical result | `0a47cc3da6862f261d687615344dd8e35865e53556c28d31b1758a5eda876c1d` | unchanged |
| Spec 145 campaign summary | `8bad7f90d12647f2904e208000e189ca73a5369ebb33f359a8f07a5560ca2566` | unchanged |
| Spec 145 20-fps run summary | `6d842b6b538ef112a2d0a961fda129d950a9e546769a07974049841bd8055243` | unchanged |

No Spec 127, 128, or 145 runner was invoked. No campaign process remained
after Spec 144.

## Final Boundary

Core/binding neutrality passes, but the feature cannot receive a positive
acceptance audit because FR-011 is unimplemented and the recovery-success
ratio is not trustworthy. The immutable matrix independently reports:

- telemetry: negative because combined impairment accepted 2/5;
- acoustic/audio: negative because every impaired treatment accepted 0/5;
- shared generality: withheld.

Spec 144 is therefore closed as **COMPLETE / MEASURED NEGATIVE**, not as a
successful cross-application or protected-stream validation.
