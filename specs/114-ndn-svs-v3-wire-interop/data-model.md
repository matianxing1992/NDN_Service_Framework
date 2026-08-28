# Data Model: NDN-SVS V3 Wire Compatibility and Interoperability

## ProtocolProfile

| Field | Meaning | Validation |
|---|---|---|
| version | `V2` or `V3` | exactly one selected at construction |
| groupPrefix | unversioned application group name | non-empty valid NDN Name |
| versionedSyncName | resolved group/version route | `/v=2` or `/v=3` matching version |
| syncInterestLifetime | outer Interest lifetime | V3 default 1000 ms; V2 legacy default 1 ms |
| periodicTimeout | steady-state timer | V3 default 30 s ±10% |
| suppressionPeriod | suppression upper bound | V3 default 200 ms; V2 legacy default 500 ms |
| bootstrapTime | active local epoch | nonnegative Unix seconds, not future-invalid |
| extensionProfile | optional fork extension selection | never changes core version/envelope |

The profile is immutable after route registration. Changing versions requires a
new participant so state and validation ownership cannot switch in place.

## StateVectorData

| Field | Meaning | Validation |
|---|---|---|
| name | signed Data name | exactly `/<group-prefix>/v=3` |
| content | encoded authoritative state | exactly one StateVector TLV |
| signatureInfo/value | integrity/authenticity evidence | present and accepted by configured policy |
| wire | full Data packet embedded in Interest parameters | decodes completely; no trailing bytes inside Data |

StateVectorData is structurally parsed, asynchronously validated, then decoded.
It is never merged before validation succeeds.

## StateVector

| Field | Meaning | Validation |
|---|---|---|
| nodeEntries | canonical-name-ordered map | no malformed or duplicate ambiguous name entry |
| epochs | bootstrap/sequence tuples per node | all tuples preserved and ordered deterministically |
| bootstrapTime | session epoch in Unix seconds | nonnegative; whole vector rejected if any > now+86400 |
| sequenceNumber | published sequence in epoch | positive; zero is absence only |

### State transition

```text
untrusted bytes
  -> structurally decoded envelope
  -> validated StateVectorData
  -> semantically valid StateVector
  -> compared/merged authoritative local vector
  -> callback + timer transition
```

Any failure before merge terminates the transition without partial state.

## ExtensionEnvelope

| Field | Meaning | Validation |
|---|---|---|
| blocks | TLVs following StateVectorData | each is length-bounded and independently parsed |
| known blocks | MappingData or RepairData | handled only by SVSPubSub extension owner |
| unknown blocks | unrecognized non-core extensions | ignored without affecting core state |
| extension transaction | pending extension mutations | commits all-or-none after successful parse |

Core validation does not confer authority on extension contents. A malformed
known extension records failure and applies no extension mutation; validated
core state remains a separate committed transition.

## BootstrapEpoch

| State | Trigger | Next state |
|---|---|---|
| absent | new participant without restored value | create current Unix-seconds value |
| restored | caller supplies valid persisted value | reuse value |
| active | local publications increment sequence | retain value |
| invalid restore | negative/future-invalid caller value | construction fails; no silent replacement |
| lost | application cannot restore | create new current value and new epoch |

## InteropCandidate

| Field | Meaning |
|---|---|
| candidateId | digest of all bound identities/configuration |
| ndnSvsCommit | exact Experimental source OID |
| dirtyState | target worktree cleanliness and tracked diff digest |
| ndnCxx/NFD/Boost | dependency versions and binary identities |
| independentPeer | exact NDNts package/source and lockfile digest |
| protocolConfig | version, timers, signer/validator, extension mode |
| fixtures | positive/negative golden vector digests |
| cells | unique 0%/5% MiniNDN repetitions |
| consumerBuild | installed headers/library and NDNSF binary hashes |
| verdict | pass/fail with immutable evidence references |

Candidate state is monotonic:

```text
draft -> unit-qualified -> interop-qualified -> network-executed -> audited
  \-----------------------------------------------------------> failed
```

A failed candidate never returns to a passing state. Corrections create a new
candidate identity.
