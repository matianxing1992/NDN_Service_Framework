# Implementation Plan: NDN-SVS V3 Wire Compatibility and Interoperability

**Branch**: `Experimental` | **Date**: 2026-07-16 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from
`specs/114-ndn-svs-v3-wire-interop/spec.md`

## Summary

Replace the current hybrid protocol—V2 Sync Interest name and raw
StateVector parameters carrying V3 bootstrap tuples—with two explicit protocol
profiles. V3 becomes the Experimental default and uses `/v=3`, a signed
State Vector Data packet embedded in ApplicationParameters, V3 timers and
atomic validation. V2 remains an explicit isolated compatibility profile.
Mapping/Repair stay as trailing fork extensions rather than core V3 content.
Byte-level vectors, an independently pinned NDNts peer, MiniNDN, and focused
NDNSF regressions close the feature.

## Technical Context

**Language/Version**: C++17 for NDN-SVS; JavaScript/Node.js 22-compatible
interop peer; Python 3 for MiniNDN orchestration

**Primary Dependencies**: ndn-cxx 0.9.0, Boost 1.71, NFD, MiniNDN, exact pinned
`@ndn/svs` package/source identity

**Storage**: Existing in-memory DataStore for test publications; caller-owned
persistence of bootstrap time; immutable result directories for evidence

**Testing**: Boost.Test unit/contract tests, byte-level packet fixtures,
standalone C++/NDNts interop processes, MiniNDN controlled-loss campaigns, and
focused NDNSF Spec 112/113 regressions

**Target Platform**: Ubuntu 20.04 local fork baseline with ndn-cxx 0.9.0 and
Boost 1.71; MiniNDN namespaces using NFD

**Project Type**: Cross-repository C++ protocol-library change with JavaScript
interop fixture and Python network harness

**Performance Goals**: No throughput claim. V3 protocol timers are normative;
bounded interop workloads must converge within the 60-second acceptance window.

**Constraints**: Preserve Spec 113 commits and source compatibility where
possible; no remote mutation; no automatic downgrade; no host-NFD final claim;
no private extension inside core V3 State Vector Data; no rerun of a failed
formal candidate under the same identity

**Scale/Scope**: Core, SVSync/SVSyncShared, SVSPubSub, security options, wire
tests, one independent V3 peer, six formal MiniNDN cells, focused affected
NDNSF validation, and the narrow shared GUI environment adapter that otherwise
injects a non-standard 1 ms suppression default

## Constitution Check

- **Canonical Dynamic Runtime**: PASS. This changes the NDN-SVS dependency and
  does not reintroduce removed NDNSF service APIs or naming.
- **Security Is Part Of The Data Path**: PASS WITH DESIGN GATE. V3 validation
  must target the embedded Data and fail atomically; no outer-Interest-only
  bypass is permitted.
- **CodeGraph First, Source Verified**: PASS. CodeGraph established the current
  constructors, sender/receiver paths, validators, extensions, and tests before
  planning.
- **Spec-Driven Durable Work**: PASS. Spec 114 owns requirements, contracts,
  ordered tasks, audit, and convergence.
- **Verify With The Right Scope**: PASS. Unit/golden/interop tests precede
  MiniNDN; host NFD may be a smoke diagnostic only.
- **GSD Statefulness**: PASS WITH RECORDED WARNING. GSD health has only an
  unrelated stale Spec 111 worktree warning; Spec 114 does not remove it.

## Design Decisions

### 1. The webpage specification is normative; implementations are oracles

The 2025-01-14 SVS V3 specification defines the target. Current official C++
master still emits V2, while NDNts explicitly separates V2 and V3 and its own
interop guide uses NDNd rather than C++ as the V3 counterpart. Therefore no
single implementation is copied blindly. Golden vectors are derived from the
specification and cross-checked against pinned independent encoders.

### 2. One explicit profile, never a hybrid or automatic fallback

Add `SvsProtocolVersion { V2, V3 }` and a `SyncProtocolOptions` value passed
through `SVSyncCore`, `SVSyncBase`, `SVSync`, `SVSyncShared`, and
`SVSPubSubOptions`. Experimental defaults to V3. Explicit V2 resolves the
historical `/v=2`, raw StateVector, outer-Interest signing, 1 ms lifetime, and
500 ms suppression behavior as one retained profile. V3 resolves `/v=3`,
embedded Data, 1 second, and 200 ms. Optional timer overrides remain explicit
and observable; they never change the selected framing.

Do not listen on the unversioned base prefix and infer version from payload.
Each participant registers its selected versioned route. Do not dual-publish,
retry in the other version, or share authoritative state between profiles.

### 3. V3 encoding and validation share one envelope codec

Introduce a small internal codec in `ndn-svs/sync-protocol.cpp/.hpp`:

```text
encodeV3(groupVersionName, StateVector, trailingExtensions, dataSigner)
  -> Interest /group/v=3/<ParametersSha256Digest>
     ApplicationParameters = Data(/group/v=3, Content=StateVector, Signature)
                             + trailing extension blocks

decodeV3Envelope(Interest)
  -> structural envelope {StateVectorData, trailingBlocks}
```

Production and tests call the codec through public behavior, but golden-vector
fixtures use independently fixed bytes so they cannot reproduce a codec error
by construction. `decodeV3Envelope` performs structural/name checks only.
Configured validation is then applied asynchronously to the embedded Data on
the Face event loop. Only validated Content may be sent to serial or parallel
state-vector decoding and merging. The parallel worker never receives an
unvalidated authoritative vector.

V2 retains its current encoder/parser behind a separate codec function. Shared
post-validation compare/merge/state-machine logic remains common.

### 4. Security targets the embedded Data

V3 uses `SecurityOptions::dataSigner` for State Vector Data. When a Data
validator is configured, it validates that same Data and an invalid result
blocks the entire packet. When no trust validator is configured, the participant
performs structural signed-envelope checks and reports `unverified` as the
effective policy; it never logs a false `validated` result. V2 continues to use
`interestSigner` and Interest validation. Tests use a pinned shared HMAC profile
for deterministic cross-language validation and fixed wire bytes, plus a
KeyChain-backed profile for normal C++ paths. No debug bypass is added.

### 5. Invalid vectors have one atomic rejection boundary

The decoder returns a typed outcome or throws a common caught decode error for
all malformed-vector cases. Sequence-zero entries are invalid; absent entries
remain logically zero. A bootstrap time greater than `now + 86400s` invalidates
the complete vector. Structural, signature, vector, and known-extension failure
metrics are separate. Rejection occurs before `mergeStateVector`, callbacks,
recorded-vector mutation, or suppression-state transition.

### 6. Bootstrap persistence is caller-owned, not a hidden filesystem

`SyncProtocolOptions::bootstrapTime` is optional. If present and valid, Core
reuses it; otherwise Core creates the current Unix-seconds value. Existing
`getBootstrapTime()` exposes the active value. This satisfies restart reuse
without introducing a second persistence engine or filesystem policy inside a
protocol library.

### 7. Mapping and Repair are a trailing extension profile

V3 ApplicationParameters begins with the standard embedded Data. MappingData
and RepairData, when enabled, follow it as distinct TLVs. Core returns trailing
blocks without interpreting them. SVSPubSub owns known extension parsing and
commits extension state only after the core vector is validated. Unknown blocks
are skipped according to the extension contract. Malformed known blocks are
reported and dropped atomically at the extension layer; they cannot mutate
core state.

Current whole-parameters LZMA wrapping is disabled for V3. V2 retains its
existing build-time behavior. A future compressed V3 profile needs a separate
specification, version/capability signal, and independent interop tests.

### 8. Validation is layered and candidate-bound

Validation order:

1. independent fixed-byte codec tests and malformed-vector tests;
2. serial/parallel equivalence and timer/state-machine unit tests;
3. explicit V2 regression vectors;
4. standalone C++ ↔ pinned NDNts V3 bidirectional smoke;
5. immutable MiniNDN candidate: three 0% and three 5% cells, each run once;
6. rebuilt NDNSF focused Spec 112/113 regressions;
7. code-aware audit, traceability, and convergence.

NDNts timers are explicitly set to the V3 specification values; its package
defaults are not treated as normative. Failed formal cells remain failed under
their identity. A correction creates a new candidate and full six-cell set.

### 9. Migration and rollback preserve Spec 113

The V3 work is added above `Experimental@c34c04d`. No Spec 113 commit is
rewritten. Implementation commits are split into: protocol options/codec,
security/receive state machine, extension adaptation, and interop evidence.
Before changing the default, explicit V2 vectors must pass. Rollback selects V2
or reverts the new V3 commits; it never removes the Spec 113 reliability fixes.
No remote branch or master movement is part of Spec 114.

The added constructor/options plumbing promises source compatibility only, not
binary ABI compatibility. Every dependent C++ target, including NDNSF, must be
rebuilt against the installed candidate. NDNSF exposes an explicit
`NDNSF_SVS_PROTOCOL_VERSION=v2|v3` rollback selector that populates the single
`SVSPubSubOptions::syncProtocol` value; V3 remains its default after migration.
The existing ServiceUser and ServiceProvider integration currently calls
`setMaxSuppressionTime(1ms)` even when the operator did not set an environment
variable. That call must become conditional: an absent
`NDNSF_SVS_MAX_SUPPRESSION_MS` preserves the profile-resolved value (200 ms for
V3), while an explicitly supplied value remains a supported, logged
experimental override. The shared NDNSF-DI GUI environment adapter must stop
manufacturing `1` as its default, expose the selected protocol version, and
emit the suppression variable only when the operator entered an override.
English and Chinese operator documentation must describe the change together.

### 10. No Sync Ack

V3 never replies to a Sync Interest with Data. Outdated vectors cause a bounded
suppression transition and, when required, a later Sync Interest. Unit and
interop capture tests assert zero Sync Ack Data for valid, invalid, outdated,
and suppression-triggering inputs.

## Implementation Phases

1. **Baseline and fixed vectors**: bind source identities, capture failing hybrid
   packet, add independent V2/V3 positive and negative fixtures.
2. **Protocol foundation**: add profile/options, versioned routes, codec, V3
   timers, bootstrap injection, and production tests.
3. **Validation/state safety**: embedded-Data signing/validation, atomic invalid
   vector handling, sequence-zero and future-time boundaries, parallel parity.
4. **Extension adaptation**: move Mapping/Repair after Data, unknown/malformed
   handling, and V3 compression prohibition.
5. **Independent interop**: pin NDNts, build deterministic C++/JS peers, and run
   bidirectional standalone contract tests.
6. **Network and consumer evidence**: six immutable MiniNDN cells, reinstall
   candidate, rebuild NDNSF, run focused Spec 112/113 regressions.
7. **Review and closure**: concern commits, traceability, audit, convergence,
   and completion summary; no merge/push.

## Failure, Rollback, And Stop Rules

- A failing test is fixed before proceeding to the next dependent phase; do not
  restart all prior tests after each small fix. After all first-pass phases have
  run, execute the complete validation set once against the final candidate.
- Do not start MiniNDN until unit, golden, negative, V2, and standalone
  bidirectional interop gates pass.
- Any signature bypass, state mutation on invalid input, cross-version state
  application, uncaught decode exception, Sync Ack emission, or golden-vector
  mismatch is a stop.
- Any 0% MiniNDN loss or process restart is a failed candidate. A 5% failure is
  retained as a negative result and also blocks the stated acceptance claim.
- Preserve the pre-feature Experimental ref and candidate manifest before any
  implementation commit. Do not reset, clean, or rewrite the dirty NDNSF
  control repository.
- Rollback means selecting explicit V2 or reverting Spec 114 commits in reverse
  dependency order; never revert the four Spec 113 commits as collateral.

## Project Structure

### Documentation (this feature)

```text
specs/114-ndn-svs-v3-wire-interop/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── tasks.md
├── traceability.md
├── completion-summary.md
├── checklists/
├── contracts/
└── evidence/
```

### Target NDN-SVS source and tests

```text
../ndn-svs/ndn-svs/
├── core.cpp/.hpp
├── sync-protocol.cpp/.hpp
├── version-vector.cpp/.hpp
├── security-options.cpp/.hpp
├── svsync-base.cpp/.hpp
├── svsync.hpp
├── svsync-shared.hpp
├── svspubsub.cpp/.hpp
└── tlv.hpp

../ndn-svs/tests/
├── unit-tests/
│   ├── core.t.cpp
│   ├── version-vector.t.cpp
│   ├── v3-wire.t.cpp
│   └── svspubsub.t.cpp
├── fixtures/svs-v3/
└── interop/
    ├── cpp/svs3-peer.cpp
    └── ndnts/
        ├── package.json
        ├── package-lock.json
        └── svs3-peer.mjs
```

### Control, MiniNDN, and NDNSF validation

```text
Experiments/
├── NDN_SVS_V3_Interop_Minindn.py
└── spec114_candidate_manifest.py

tests/python/
├── test_spec114_candidate_manifest.py
├── test_spec114_svs_v3_tools.py
└── test_ndnsf_di_tk_widgets.py

ndn-service-framework/
├── ServiceUser.cpp
└── ServiceProvider.cpp

NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/gui.py

results/spec114-svs-v3/<candidate>/
```

**Structure Decision**: Product protocol code and independent fixtures belong
to the clean NDN-SVS target repository. The already dirty NDNSF repository owns
Spec Kit control, MiniNDN orchestration, candidate evidence, and affected
consumer validation. Generated dependencies and formal results are not added to
the product diff.

## Post-Design Constitution Check

All five constitution principles pass. The design introduces one version
selector and one envelope codec rather than parallel state machines, keeps
security on the actual V3 signed object, isolates extensions by ownership,
preserves explicit migration/rollback, uses MiniNDN for final network evidence,
and binds all claims to immutable candidate identity. No exception or complexity
waiver is required.
