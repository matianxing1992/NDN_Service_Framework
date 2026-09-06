# Implementation Plan: Request-Scoped Confidentiality and Epoch Revocation

## Online authorization recovery amendment (2026-09-05)

T020 reviews the Experimental dependency as a reusable library. Detach CK
waiter batches before application callbacks; fence parameter fetch/retry and
validation generations; install name/digest-checked parameter candidates
atomically; serve current public parameters only under their canonical name;
bind decrypted CK caching to scheme/parameters/private key/ciphertext. Restore
the original no-argument parameter-fetch entry and document silent cancellation,
Face-thread ownership and mandatory dependent rebuilds for changed class layouts.
Acceptance requires reproduced negatives, the complete NAC suite, existing
example builds, rebuilt NDNSF integration and representative live grant/revoke
paths. See `evidence/nac-abe-compatibility-review-20260905.md`.

T017 repairs the existing pending-rotation state machine: reconcile before
duplicate-target return or any grant mutation, and durably advance the epoch
before a recovery attempt changes parameter identity. Previously published
failure statuses remain immutable; failed retries retain the withdrawal.
T018 validates late App-owned permission renewal after startup retries expire
and makes the MiniNDN gate retain all failed requests and return a failing exit
code for a failed gate. Detailed audit, negative reproductions and final results
live in `evidence/online-authorization-audit-20260905.md`.

The late-grant campaign further reproduced a first-DKEY constructor deadlock:
an unprovisioned User cannot reach the App permission API at all; Provider has
the same wait. Remove both blocking loops, initiate the existing asynchronous
Consumer fetch, retain permission/status checks and replace the former implicit
initial-DKEY prerequisite with an explicit real-User admission check. Preserve
hybrid key-unwrap error callbacks independently of the success closure in both
roles; a valid-permission/no-DKEY regression reproduces both missing guards.
No polling thread or new wire/API is added. Real-constructor timeout and
unauthorized-publication/execution tests plus the full network campaign gate
this startup change. Scenario workloads must also drain for at least their
request timeout rather than treating teardown as successful authorization.

T019 closes a further dependency-side revocation gap found during final MiniNDN:
an asynchronous content completion can outlive `Consumer::clearCache()` and
reach crypto with a cleared DKEY; CK callbacks can repopulate invalidated state.
Capture/check the existing cache generation in content and CK completion/error
callbacks before any state mutation or application callback. Convert OpenABE's
enum failures into the existing `NacAlgoError` contract. Tests use real segmented
fetches held across invalidation, verify fresh recovery, and exercise invalid
CP/KP key input. This remains NAC-ABE-owned; no NDNSF wire or permission-policy
change is needed. Rebuild the exact prefix and reverify resolution, expanded
native gates and the complete network campaign. Dependency publication is T014.

**Branch**: `UAV-Experimental` | **Date**: 2026-09-01 | **Spec**: `spec.md`

## Summary

T022 authorized follow-up: merge official NAC58f3948 into Experimental while
preserving local repairs. First add regression evidence for CacheProducer's
ignored non-default segment limit; verify CP/KP cold and warm cache semantics
and segmented Consumer object/exact-segment retrieval. Use a new build/prefix
and clean NDNSF build at-j2, verify actual linkage, full NAC and expanded native
gates, then the18-scenario MiniNDN campaign. No external publication. Keep
the prior85547eb prefix and frozen evidence intact for matched rollback.

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

## Amendment 2026-09-05 — implementation guidance (FR-039–FR-041)

Order and scope follow `tasks.md` Phase 7 (T016 → T015 → T014).

### T016 (FR-040, hierarchical trust anchor) — no framework change expected

The runtime validates Controller status through `MessageValidator`
(`ValidatorConfig` trust schema). A hierarchical schema (root anchor,
`hierarchical` rule over an intermediate, Controller certificate signed by
the intermediate) exercises the same install path with a deeper chain.
Fixture: generate root/intermediate/Controller identities in-test, write a
temporary schema file, drive the existing live-install LocalMock surface;
negative cases sign status with a chain-external identity and with an
intermediate that does not chain to the anchored root. Reuse
`ConfiguredTrustSchemaControlsControllerStatusValidation`'s shape.

### T015 (FR-039, opt-in persistent runtime status)

- Opt-in env `NDNSF_PERSIST_RUNTIME_STATE` (path via
  `NDNSF_RUNTIME_STATE_DIR`, default under `~/.local/state/ndnsf/`); unset
  keeps today's process-local semantics unchanged.
- New `RuntimeStatusStore` mirrors `ControllerGenerationStore`'s durability
  pattern (magic header, binary wire, `.tmp.<fence>` + atomic rename, corrupt
  → fail closed), but is a *reader/writer on one runtime process*, so it
  needs no cross-process writer fence; it stores one record per service:
  accepted `PolicyStatusData` wire, bound public-parameter name + digest,
  ControllerVersion, install time. File mode 0600, directory 0700; the same
  per-identity file naming used by the Controller generation store.
- Install path (`ServiceUser`/`ServiceProvider` status acceptance): after an
  accepted status becomes authority, append/overwrite that service's record.
  On startup, when enabled: load records, verify each signature against the
  configured trust anchor (unverifiable → discard that service, fail closed
  for it only), skip expired records and any record whose version the
  process has not itself seen as superseded; then start the normal
  `PolicyRefreshCoordinator` bootstrap, but seed initial authority from the
  recovered records so equal-version traffic may proceed while the bounded
  confirmation refresh runs. The Controller's signed answer on refresh is
  authority (higher replaces, equal idempotent). Never persist DKEY or
  request-key material.
- Restart-of-runtime tests reuse the LocalMock/LocalController surface with
  two sequential `ServiceUser`/`ServiceProvider` constructions over the same
  store directory (mirroring `ServiceControllerRestoresEveryRevocationKindAfterRestart`).

### T014 (FR-041, upstreaming) — local package first

Split description only (history of `b1c9c4f` stays intact): (1) the
one-hunk freshness fix in `attribute-authority.cpp` DKEY segment publication
plus its rationale comment; (2) the API-contract extension
(consumer/producer/param-fetcher/abe-support) mapping each new symbol to its
Spec179 requirement. Rebuild-and-rerun of RV-U20/RV-U21 happens only after
the upstream commit exists (external gate).

Current package: `evidence/nac-abe-delivery-20260905.md` includes all four
commits through85547eb, mandatory T019/T020 repairs and ABI migration guidance.
The two sections are review descriptions, not independently qualified branches.
The tested base is personal fork master; official UCLA-IRL master includes two
further fixes plus their merge commit. `evidence/nac-abe-official-comparison-20260905.md`
records historical ahead/behind counts and conflict-free static merge previews.
T022 subsequently qualifies mergedc3aafa6 with NAC46, NDNSF183/74 and18 MiniNDN
scenarios; see `evidence/nac-abe-official-merge-20260905.md`. User requests no PR;
publication is not scheduled.
A standalone bundle preserves the exact tested source/history.

### T021 — Controller grants for both runtime roles

The user's permission scope is Controller-authorized service use (User,
`/PERMISSION/<service>`) and service offering (Provider, `/SERVICE/<service>`).
Keep this authority boundary in the existing `grant()` API. The example gains
an explicit grant-role option defaulting to User; Provider mirrors the existing
App-owned delayed permission refetch. No framework permission-polling feature
or new wire field is needed.

Provider/B starts unprovisioned while Provider/A serves as a control. Controller
grants B, B explicitly renews permissions, and User/B renews its provider table
before successfully targeting B. Verify no pre-grant Provider publication,
target-only DKEY refresh and all target/control terminal rows without censoring.
The late variant drops UDP only in the isolated Provider/B namespace until the
observed final permission timeout, restores the exact rule in finally, then
requires timeout < grant < renewal < successful service. Pure evaluator tests
must reject missing/forged transitions and any target/control failure. Rebuild
only the changed example targets against the already verified matching NAC
prefix; run both new network variants and the existing User grant control.
The completed T02016-case cohort stays frozen at de1eb508.

First live T021 probes additionally expose two bounded defects: benchmark
request paths in `App_User.cpp` ignore the explicit provider list, and User
permission installation confuses added provider routes with new service ABE
attributes. Repair the benchmark adapter through existing provider-specific
overloads and compare service sets in `ServiceUser::applyPermissionResponse`;
extend the existing real Controller integration grant test. Provider status
advances already trigger permission revalidation, so actual post-grant fetches
may precede the App timer; require actual renewal and idempotence, including
the late timeout ordering. Revalidate28 component tests, the full selected
native183/72 suites and all18 network scenarios after these runtime changes.
The second live probe further requires central ACK membership in a nonempty
explicit Provider set; Controller permission alone does not override caller
selection. Empty-list discovery stays supported. Final acceptance at994018ac
passes all18 scenarios,188 assertions, both User grant gates and33 matching
artifact hashes; the evidence report closes T021 and retains both network reds.
