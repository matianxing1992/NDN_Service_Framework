# Research: SVS PubSub Payload Interoperability

## Decision 1: Test SVS-PS semantics, not raw packet visibility

**Decision**: A direction passes only after the remote subscriber callback
returns an application name and payload matching the corpus manifest.

**Rationale**: The existing SVS V3 test already proves StateVector parsing.
SVS-PS additionally requires mapping retrieval, outer Data retrieval,
decapsulation, segment reassembly, and validation.

**Alternatives considered**: Packet capture alone and equal final StateVectors
were rejected because neither proves application payload recovery.

## Decision 2: Use each implementation's public PubSub API

**Decision**: Use C++ `SVSPubSub::publish/subscribe` and TypeScript
`SvPublisher.publish` plus `SvSubscriber.subscribe` with an in-memory NDNts
DataStore.

**Rationale**: Hand-coded Data fetches would test an NDNSF adapter instead of
the claimed library interoperability.

**Alternatives considered**: A shared custom wire codec and direct exact-name
fetcher were rejected as compatibility shims.

## Decision 3: Keep a fail-first standalone gate

**Decision**: Run one bounded standalone payload exchange before any MiniNDN
matrix. A failure stops the matrix and records the first incompatible stage.

**Rationale**: Source inspection already exposes a credible format risk. The
official SVS-PS specification uses a Mapping query without bootstrap time and a
MappingEntry containing `SeqNo + ApplicationName`; the current C++ source
appears to extend both Mapping and publication identity with bootstrap time.
The harness must measure, not hide, that difference.

**Alternatives considered**: Running the full loss matrix immediately was
rejected as expensive and diagnostically weak. Patching either peer in the test
was rejected because it would not prove native interoperability.

## Decision 4: Prove binary identity with length and SHA-256

**Decision**: Compare application name, length, and SHA-256 for every case and
direction; include a deterministic payload containing zero and invalid UTF-8
bytes.

**Rationale**: Text conversion and length-only comparisons can conceal byte
corruption.

**Alternatives considered**: Base64 text equality and callback counts were
rejected as weaker evidence.

## Decision 5: Keep network acceptance small and honest

**Decision**: After standalone success, run one 0% and one 5% MiniNDN cell with
bounded deadlines and packet captures. Preserve failures without automatic
reruns.

**Rationale**: This is a compatibility gate, not a reliability estimate or
performance campaign. Repetitions are not needed until the protocol path is
known to work.

## Sources

- Official SVS-PS specification:
  `https://named-data.github.io/StateVectorSync/PubSubSpec.html`
- Pinned NDNts package source and type declarations under
  `examples/interop/ndn-svs-v3/ndnts/node_modules/@ndn/svs/`
- Current local NDN-SVS public API and implementation under
  `/home/tianxing/NDN/ndn-svs/ndn-svs/`
