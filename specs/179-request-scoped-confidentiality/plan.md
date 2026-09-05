# Implementation Plan: Request-Scoped Confidentiality and Epoch Revocation

**Branch**: `UAV-Experimental` | **Date**: 2026-09-01 | **Spec**: `spec.md`

## Summary

Replace service-wide response-key wrapping with a request-scoped confidentiality
path. NAC-ABE remains the pre-selection discovery gate. After ACK/Selection,
the User creates `K_input` and `K_response`; Selection carries an RSA-OAEP
envelope for the selected Provider, and Response Content uses AES-GCM with
binding AAD and unique per-event/segment nonces. Every authorization-bearing
message carries a compact `ControllerVersion` pair; Controller-signed status
drives certificate/ABE refresh, revocation, and cache invalidation across
Controller restarts.

## Technical Context

**Language/Version**: C++17/Boost.Test, ndn-cxx, NDN-SVS, NAC-ABE; Python 3.8 MiniNDN harness

**Primary Dependencies**: ndn-cxx security certificates, existing NAC-ABE Consumer/Producer, OpenSSL/RSA-OAEP and AES-GCM primitives already used by NDNSF, NDN-SVS controller/status Data. NAC-ABE cache invalidation requires the small upstream-facing `Consumer::clearCache()`/generation-fence patch recorded in `evidence/nac-abe-consumer-cache-reset.patch`; builds using it must pass `--nac-abe-prefix=<prefix>` so headers and the shared library cannot be mixed with `/usr/local`.

**Storage**: Existing bounded in-memory selection/replay/key caches plus the durable atomic Controller generation state and writer lease. Runtime restart safety is achieved by clearing non-persistent invocation material and re-establishing current signed status; a persistent request-key WAL is not introduced by this feature.

**Testing**: Normative RV-U/RV-I inventory in `validation-matrix.md`; C++ deterministic unit tests, real Controller/User/Provider integration tests, Python cryptographic contract tests, MiniNDN two-User/two-Provider network tests, and existing authorization regressions

**Target Platform**: Linux MiniNDN and normal NDNSF C++ deployments

**Project Type**: Network service framework/library protocol extension

**Performance Goals**: One additional request-level key-envelope operation per selected invocation; no unbounded retries or service-wide key fan-out; bounded refresh latency configurable by deployment

**Constraints**: No plaintext input/result in wire payloads; exact non-zero ControllerVersion matching; one active writer per Controller identity; RSA encryption certificate for initial envelope path; AES-GCM nonce uniqueness; no retroactive revocation claim; preserve existing selection/token/terminal-owner semantics

**Scale/Scope**: Normal, large-response, and streaming/segmented invocations; multiple concurrent Users/Providers; one active key bundle per request attempt

## Constitution Check

- **Security boundaries**: PASS — keys are recipient-specific; ABE remains discovery-only; controller status is signed; failures are closed.
- **Existing protocol ownership**: PASS — reuse Request/ACK/Selection/Response, certificates, NAC-ABE, replay stores, and named Data; no parallel lifecycle.
- **Test-first and evidence**: PASS — unit → integration → MiniNDN order with negative tests and redacted evidence.
- **Branch discipline**: PASS — implementation targets `UAV-Experimental`; temporary spec branch is not a release branch.
- **Claim discipline**: PASS — forward-only revocation and old-key limitations are explicit.

## Architecture and Data Flow

```text
Controller status Data (signed, generation G, epoch E)
        |
User/Provider refresh + cache invalidation
        |
User -> Request: ABE(discovery descriptor) + ControllerVersion(G,E)
      + User encryption certificate name/digest
Provider candidates -> ACK: certificate metadata + ControllerVersion(G,E)
User creates K_input/K_response
User -> Input Data: AES-GCM(K_input, input), exact name + User signature
User -> Selection: Input Data name + encrypted keys + ControllerVersion(G,E)
Selected Provider -> Response: AES-GCM(K_response, result, ControllerVersion-bound AAD)
                     + Provider-signed NDN Data
User decrypts; other Users/Providers fail authentication
```

The Selection digest covers canonical non-secret selection metadata and is
included in AAD. Key ciphertext is not included in its own digest input, which
avoids circular binding.

## Implementation Phases and Gates

### Phase 0 — Contract and inventory

Freeze field names, algorithms, nonce lengths, ControllerVersion rules, certificate digest
encoding, error vocabulary, and compatibility telemetry. Inventory all current
`HybridMessageKey`, large-response, stream, Targeted, and policy-epoch callers.

### Phase 1 — Cryptographic primitives and wire contracts

Add canonical binding/AAD encoding, RSA-OAEP key-envelope encoding, AES-GCM
request/result helpers, nonce registry, and typed diagnostics. Unit tests must
cover tampering, wrong recipient, nonce reuse, and canonicalization before
runtime wiring.

### Phase 2 — Normal Request/ACK/Selection/Response integration

Carry certificate advertisements in ACK, create request keys at Selection,
consume the envelope once in the selected Provider, and encrypt/decrypt normal
Response Content. Preserve signing, permission, token, replay, and terminal
owner checks.

### Phase 3 — Large and streaming integration

Remove service-wide permission-DKEY wrapping from large responses. Reuse one
invocation response key only with unique segment nonces and segment-bound AAD.
Extend Targeted bootstrap/refill bindings without bypassing ControllerVersion or recipient
validation.

### Phase 4 — Controller generation, compact version refresh, and revocation

Persist a strictly increasing generation timestamp before Controller issuance,
require one active writer, publish/consume signed status Data, and require exact
non-zero ControllerVersion equality against the accepted status for the exact
service. An authenticated newer message triggers the per-service refresh
coordinator even if another service has already installed that same or a newer
process-wide version. The coordinator permits one in-flight exact-version fetch
and retains only bounded highest-candidate/retry state, without comparing the
generation timestamp with local wall time; an older message is rejected with the
receiver's compact service version. Data may arrive from any cache/Provider but
remains authoritative only through the Controller signature. Installation of a
validated status into authorization state, refresh state, and cache invalidation
is one transaction: rejection at any step preserves the previous authority and
cache contents. Unit tests close every
version/status/persistence/lease/policy/cache/state decision, including the
no-installed-status fail-closed branch and the rule that a message-carried
version is only a refresh hint. Integration tests then revoke a User,
Provider, certificate, User-`/PERMISSION` service grant, and
Provider-`/SERVICE` service grant at every invocation cut point, with matched
unaffected attribute/identity/service
controls, signed permission-renewal denial, timestamped status
publication across Controller restarts, exact-version hint refresh,
reauthorization, restart, Controller-unavailable, hintless scheduled
pre-expiry refresh, Targeted, stream, and concurrent-refresh cases. The
normative inventory is `validation-matrix.md`. The Controller authority gate is
separate from receiver predicates: unit tests cover mutation/commit rollback,
exact status publication/replacement, scope-aware issuance, refresh and cache
invalidation; component tests cross the real Controller-to-runtime path and
execute every distinct enforcement owner, cache/terminal path, and recovery
class with paired unaffected controls. Target kinds are distributed across
those paths; identical implementations are not multiplied into a full Cartesian
campaign.

The wire migration adds the existing NAC-ABE `authorizationAttribute` identity
to `SERVICE_AUTHORIZATION` targets and runtime `AuthorizationSubject` checks:
`/PERMISSION/<service>` for User grants and `/SERVICE/<service>` for Provider
grants. ControllerVersion advances for every authorization-table change, but
the ABE generation is identified separately by the exact public-parameter Data
name and digest. For an authorization-reducing change, the current NAC-ABE
library requires a fresh global master-secret/public-parameter generation and
filtered DKEY reissuance to every still-authorized identity; new ciphertext
never uses the old generation. For a grant-only change, the Controller retains
the existing controller-private master secret (master key) and public
parameters; neither is sent to participants. Because the current KP-ABE
implementation stores one monolithic policy/DKEY per identity, it replaces
only that identity's complete current-attribute policy and makes one complete
replacement DKEY available on one target-only lazy DKEY fetch (normally
initiated after the target installs signed status, with explicit fetch as a
fallback), rather than patching one fragment. Unaffected identities retain their current-generation
DKEYs and do not refetch; there is no global DKEY fan-out. The target's old
same-generation DKEY remains limited to its previous attributes until the
replacement is installed. User and
Provider runtimes check both peer and local attribute
authorization at the boundaries in `data-model.md`. Attribute-local rekey is
deferred until NAC-ABE exposes and tests that capability. This is a breaking
change to the uncommitted Spec179 status
format, so legacy attribute-less service targets are rejected rather than
guessed. Identity-wide and exact certificate targets remain attribute-
independent. Post-Selection retry/reselection stays disabled unless the
application explicitly supplies idempotency or deduplication semantics.

### Phase 5 — MiniNDN security matrix and migration gate

Run two Users, two Providers, repeated Controller restarts, stale versions, revoked identity,
tampered Selection/Response, replay, stream nonce reuse, and compatibility-mode
cases. Compatibility is opt-in, observable, bounded, and removable.

## Project Structure

```text
ndn-service-framework/
├── ndn-service-framework/
│   ├── ServiceUser.cpp/.hpp
│   ├── ServiceProvider.cpp/.hpp
│   ├── ServiceController.cpp/.hpp
│   ├── NDNSFMessages.cpp/.hpp
│   ├── ControllerVersion.cpp/.hpp
│   ├── PolicyStatus.cpp/.hpp
│   ├── ControllerGenerationStore.cpp/.hpp
│   ├── PolicyRefreshCoordinator.cpp/.hpp
│   ├── RevocationState.cpp/.hpp
│   ├── RequestConfidentiality.cpp/.hpp
│   └── HybridMessageCrypto.cpp/.hpp
├── tests/unit-tests/
│   ├── request-scoped-confidentiality.t.cpp
│   ├── controller-revocation-policy.t.cpp
│   ├── controller-revocation-state.t.cpp
│   └── generic-dynamic-api-crypto-auth.t.cpp
├── tests/integration-tests/
│   ├── controller-version-refresh.t.cpp
│   ├── controller-revocation-flow.t.cpp
│   ├── request-scoped-selection.t.cpp
│   ├── request-scoped-response-confidentiality.t.cpp
│   └── invocation-stream-flow.t.cpp
├── tests/minindn/
├── examples/
└── specs/179-request-scoped-confidentiality/
    └── validation-matrix.md
```

**Structure Decision**: Extend the existing framework security/message layer;
keep certificate/AAD/key-envelope code reusable and keep ControllerVersion
authority separate from User/Provider caches. No application-specific policy
belongs in the core cryptographic envelope.

## Compatibility and Rollback

The old service-wide response-key path is not the default after migration. A
temporary explicit compatibility switch may be used only for rollback tests;
it must emit a counter and reject mixed key modes within one request. Remove it
after current-path MiniNDN and streaming gates pass.

## Validation Order

1. Wire/AAD/key primitive unit tests.
2. ControllerVersion, policy mutation, persistence/lease, atomic per-service
   installation, cache invalidation, lifecycle-transition, refresh-bound, and
   reauthorization unit tests (all RV-U rows).
3. Normal C++ User/Provider integration tests, including cross-service version
   isolation and exact-status refresh.
4. Risk-mapped integration tests that cover every distinct revocation target,
   enforcement owner, cache/terminal path, and recovery class with unaffected
   controls (all RV-I rows); repeat combinations only for distinct code paths.
5. Large-response, Targeted, and streaming confidentiality regressions.
6. Representative MiniNDN two-User/two-Provider security scenarios after every
   deterministic revocation row passes; do not reproduce the unit/component
   branch matrix on the network.
7. Compatibility migration, traceability, and documentation audit.

The release gate checks coverage by security decision and state transition,
not by aggregate line percentage. Any RV-U/RV-I row that is unrun or reaches a
mock-only shortcut remains missing evidence.
