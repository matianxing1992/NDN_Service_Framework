# Focused validation evidence

Date: 2026-07-16  
Final source: `Experimental@53dd1588201b967a4aa9decd3e51ade3263e0f88`

## Audit-convergence safety fixes

The final receive pipeline first inspects the V3 envelope and embedded Data,
then validates that Data, and only then performs semantic StateVector decode.
`V3ValidationPrecedesSemanticVectorDecode` proves an independently generated
packet with both a rejected signature and invalid sequence zero is attributed
to signature policy, with zero vector-decode rejection and no state mutation.
V2 retains immediate legacy decoding.

Serial and parallel paths now merge validated core state before delivering the
complete bounded extension collection once. `SVSPubSub` validates every known
MappingData/RepairData block before committing any extension mutation. The new
collection test proves a valid mapping followed by malformed repair commits
nothing, while the same mapping alone commits successfully.

## Profile and failure gates

- V3 default: 1000 ms Interest lifetime, 200 ms suppression, 30 s periodic
  interval with 0.1 jitter; explicit `1` remains observable.
- Explicit V2 stays isolated with the legacy route/envelope and 1 ms lifetime.
- V3 has no whole-parameters LZMA representation and never silently downgrades.
- Wrong names/signatures, malformed Content/NNI, duplicate core blocks, zero
  sequence numbers, future epochs, and malformed extension collections reject
  without partial state.
- `NDNSF_SVS_PROTOCOL_VERSION=invalid` exits before serving with rc=2 and
  `NDNSF_SVS_PROTOCOL_VERSION must be v2 or v3`.

## Executed results

```text
NDN-SVS full runner                         67/67 passed
TestV3Wire/ProfileDefaultsAndOverrides       1/1 passed
Spec 114 manifest tools                      3/3 passed
Spec 114 MiniNDN analysis tools              8/8 passed
NDNSF TargetedInvocation                    18/18 passed
NDNSF NdnSvsSmoke                            2/2 passed
NDNSF MessageValidator focused               2/2 passed
Python focused                              41 passed, 3 skipped
GUI/profile round-trip                      16/16 passed
```

The three Python skips are deliberate MiniNDN-exclusive checks; the accepted
consumer campaign executes those real network paths. `git diff --check` passed
for the target tree.
