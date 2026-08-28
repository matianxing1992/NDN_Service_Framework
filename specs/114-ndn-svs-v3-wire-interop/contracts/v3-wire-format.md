# Contract: SVS V3 Core Wire Format

## Names

```text
State Vector Data Name: /<group-prefix>/v=3
Sync Interest Name:     /<group-prefix>/v=3/<ParametersSha256DigestComponent>
Publication Interest:   /<node-prefix>/<group-prefix>/t=<bootstrap>/seq=<seq>
```

The parameters digest is computed after the complete ApplicationParameters are
installed. Receivers require the embedded Data name to equal the selected
versioned sync name exactly.

## ApplicationParameters grammar

```text
ApplicationParameters = TLV-TYPE(36) TLV-LENGTH
                        StateVectorData
                        *ExtensionBlock

StateVectorData = Data packet
  Name      = /<group-prefix>/v=3
  Content   = StateVector
  Signature = configured Data signature
```

The outer Interest is not the authoritative signed object. It may carry normal
Interest fields, but signing it does not replace the embedded Data signature.

## StateVector grammar

```text
StateVector       = 201 TLV-LENGTH *StateVectorEntry
StateVectorEntry  = 202 TLV-LENGTH Name *SeqNoEntry
SeqNoEntry        = 210 TLV-LENGTH BootstrapTime SeqNo
BootstrapTime     = 212 TLV-LENGTH NonNegativeInteger
SeqNo             = 214 TLV-LENGTH NonNegativeInteger
```

- StateVectorEntry values are encoded in NDN canonical Name order.
- All tuples for a node are preserved; tuple ordering is deterministic.
- An encoded SeqNo is at least 1. Absent tuple state is treated as 0 only during
  comparison.
- BootstrapTime is Unix seconds.
- If any BootstrapTime is greater than `currentTime + 86400`, reject the entire
  vector before merge.

## Receive pipeline

```text
versioned route
  -> outer Interest structural check + parameters digest check by ndn-cxx
  -> extract first child as Data; retain trailing bytes separately
  -> exact embedded Data name/signature-structure check
  -> configured asynchronous Data validation
  -> decode exactly one StateVector from Content
  -> semantic vector validation
  -> serial/parallel common compare/merge
  -> callback/timer transition
  -> extension owner receives trailing blocks
```

## Failure contract

For wrong version, missing Data, wrong Data name, invalid signature, malformed
Content, wrong TLV order/type, sequence zero, or future bootstrap:

- no local or recorded vector mutation;
- no missing-data/update callback;
- no timer state transition based on the invalid vector;
- no extension parsing;
- no exception escape from Face or worker callbacks;
- exactly one bounded structured rejection diagnostic;
- next valid packet remains processable.

## Sender equivalence

Serial and parallel production must generate byte-identical packets given the
same vector, signature inputs, extensions, and deterministic signer. Signing in
a worker is permitted only when the signer is thread-safe or protected by the
existing signing-ownership mechanism.

Golden signature fixtures use a deterministic public test HMAC key and pin all
signing inputs. KeyChain-backed signatures are asserted structurally and by
validation outcome, not compared byte-for-byte when the algorithm is
nondeterministic.

## No Sync Ack

Receiving a Sync Interest never causes a Data response. If reconciliation is
needed, the state machine schedules or emits another Sync Interest after the
required suppression decision. Packet tests count Data emissions and require
zero Sync Ack for every receive case.
