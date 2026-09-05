# Research: Request-Scoped Confidentiality and Epoch Revocation

## Decision 1: Keep ABE at discovery boundary

The current runtime already obtains controller-distributed NAC-ABE material for
service permissions and uses policy epochs in `ServiceController`,
`ServiceUser`, and `ServiceProvider`. That mechanism is appropriate for hiding
service-level discovery metadata before a Provider is selected. It is not an
appropriate owner for a response CEK because every holder of the service DKEY
would share the decryption boundary.

**Chosen boundary**: ABE protects the discovery descriptor only. Request and
result plaintext are protected by a fresh invocation key bundle delivered after
selection.

## Decision 2: Encrypt the Selection key envelope to the selected Provider

ACK already identifies a Provider and the current constructors require an RSA
encryption certificate for NAC-ABE. The Selection packet therefore carries the
exact named Input Data reference, a canonical binding, plus an RSA-OAEP
envelope containing `K_input` and `K_response`. The selected Provider fetches
and verifies that named Data after unwrapping the envelope. The User keeps
`K_response`; the Provider never receives a service-wide response key.

Alternatives rejected:

- wrapping the response key in `/PERMISSION/<service>`: wrong recipient scope;
- encrypting to every ACK candidate: allows unselected Providers to recover
  invocation secrets;
- sending the key plaintext in Selection: violates confidentiality and makes
  Selection capture sufficient for decryption.

## Decision 3: AES-GCM with explicit binding AAD

AES-GCM is already the practical symmetric primitive for large content in the
runtime. The contract binds service, request, attempt, both ControllerVersion
fields, both certificate digests, Selection digest, and segment/event identity
in AAD. The Selection
digest is calculated over canonical non-secret Selection metadata before the
encrypted envelope is assembled, avoiding a circular digest.

One invocation may reuse `K_response` for large/streaming segments, but each
segment receives a unique nonce and segment-bound AAD. A nonce registry is
scoped to the invocation and persisted/tombstoned long enough to reject replay.

## Decision 4: ControllerVersion survives Controller restart

The Controller is the authority for authorization and revocation status. A
compact `ControllerVersion` contains a persisted generation timestamp and a
non-zero epoch within that generation. On restart, the Controller atomically
persists `max(currentUnixTimeMs, previousTimestamp + 1)` before issuance; a
single-writer lease prevents two live instances from claiming the same
identity. This avoids epoch collision after repeated restarts and tolerates a
bounded wall-clock rollback without making message send time authoritative.

Revocation is forward-looking: it prevents future issuance and future
requests. It cannot recall cached NDN Data or erase plaintext already revealed
to a legitimate old-key holder.

## Decision 5: Compact versions trigger authoritative refresh

The current implementation refreshes controller material during startup or
rejoin. Spec179 adds the two-field ControllerVersion to Request, ACK, Selection,
Response, and Targeted control messages. After authenticating the sender, a
newer pair triggers one bounded, coalesced fetch of the exact immutable
Controller-status Data named by that pair; an older pair is rejected with the
receiver's current pair. The generation timestamp is an ordering token and is
never compared with a receiver's wall clock. No peer-supplied version is
accepted without Controller signature validation.

## Decision 6: Status retrieval is source-independent and data-driven

The node fetches latest Controller status itself. The signed Data may be
returned by the Controller, another Provider, or an NDN cache because its
authority comes from the Controller signature, service scope, validity, and
monotonic ControllerVersion—not the transport peer. The message carries no
status hash/name/time beyond the compact generation timestamp and epoch. This
keeps the wire overhead fixed while preserving NDN cacheability.

## Decision 7: Current NAC-ABE withdrawal uses a new global generation

The reviewed `CpAttributeAuthority` initializes one global public-parameter and
master-secret pair in its constructor and generates each identity's DKEY from
that pair plus the identity's complete attribute list. Removing an identity or
attribute from the Controller table while retaining the same pair is
insufficient: an old DKEY can still satisfy new ciphertext encrypted under the
same attribute policy. The initial Spec179 design therefore generates a fresh
global ABE pair for every authorization-reducing change, publishes the exact
public parameters, and reissues filtered DKEYs to all remaining identities.
New ciphertext uses only the new pair. This has global refresh cost but gives
an auditable retained-old-DKEY exclusion property. A grant-only change has no
old-key exclusion requirement: it advances ControllerVersion, retains the
existing ABE pair, replaces only that identity's complete policy, and lazily
generates a replacement DKEY on the granted identity's next DKEY fetch with its
full current attribute set. Existing DKEYs remain valid and are not refetched
for unaffected identities. Reauthorization uses the current post-revocation
generation, so it cannot revive a pre-revocation DKEY. Per-attribute revocation rekey would
reduce the withdrawal cost but requires a separate NAC-ABE extension and is
not assumed here.

## Evidence and open research boundary

Relevant implementation anchors include `ServiceController` policy epoch
publication, `ServiceProvider` large-response key construction, and
`ServiceUser` response decryption/version checks. The original baseline treated
zero as a wildcard and lacked a generation timestamp; the current Spec179 slice
has corrected those predicates. It has not yet added authorization-attribute
targets or a fresh live NAC-ABE generation on withdrawal. Completion must add
deterministic Controller/revocation unit tests, real
Controller/User/Provider integration tests for every subject and lifecycle cut
point, and MiniNDN two-User/two-Provider tests. Current tests invoke
User/Provider `applyPermissionResponse` extensively and include stale-epoch,
authorization-snapshot, Targeted-token, and unchanged-Provider refresh
regressions. They do not directly cover Controller-built typed revocation
status, every cache family, in-flight enforcement ownership, Controller
unavailability, or reauthorization; no existing test directly calls
`buildUserPermissionResponse` or `buildProviderPermissionResponse`. Those
tests remain useful regressions but cannot establish the new revocation
lifecycle or cryptographic old-DKEY exclusion by themselves.
No implementation may claim retroactive revocation or confidentiality against
an identity that already possesses an old plaintext.
