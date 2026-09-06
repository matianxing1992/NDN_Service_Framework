# T020 G1 native suite block: NDN-SVS header/library API parity

**Date:** 2026-09-02  
**Subject:** replay-driver source seal `source-seal-replay-driver-20260902.json`

## Observation

The new G1 attempt did not pass. The existing
`build/integration-tests` binary reported:

```text
NdnsfDataV1SvsFlow/ProductionProviderContextUsesSvsSegments
  critical check wires has failed
Spec175InvocationStream/NormalStreamCancellationFencesLaterCallbacks
  critical check events.size() == 1 has failed [3 != 1]
```

The cancellation case passed when run alone, so that failure is an intermittent
native test result and is not accepted as qualification. The SVS case failed in
three consecutive focused runs. With bounded component logging it timed out in
`ServiceProvider::fetchCollaborationDataV1Segments` after publishing both
segments; no segment callback arrived.

## Boundary classification

This is a local build/dependency gate failure, not a Tiger scheduler or
NDNSF-DI model result. The checked-in ndn-svs source header contains
`subscribeToProducerWithCatchUp`, but the linked `/usr/local/lib/libndn-svs.so`
exports only the older `subscribeToProducer` symbol. The production source
deliberately uses the portable API because that symbol is present, while this
test requires bounded catch-up for publications observed before subscription.
The header/library feature set is therefore not the same subject.

Evidence commands:

```text
ldd build/integration-tests -> /usr/local/lib/libndn-svs.so.0.1.0
nm -D --defined-only /usr/local/lib/libndn-svs.so.0.1.0 | c++filt
  -> subscribeToProducer(...)
  -> no subscribeToProducerWithCatchUp(...)
```

## Required restart gate

Do not build a new SIF or run G4/Tiger from this G1 result. First align the
NDN-SVS header and shared library to the pinned `Experimental` subject with the
same Boost 1.71/system toolchain, or change the production/test contract only
through a reviewed source repair. Then rebuild `build/integration-tests`, run
the focused SVS case and cancellation case, rerun G1, and generate a new source
seal/G0 chain if the production source or dependency identity changes. The old
SIF and its 29/42 replay remain diagnostic only.

## Follow-up after explicit Experimental rebuild (2026-09-02)

An isolated NDNSF build was then configured with the pinned Experimental
NDN-SVS source tree and its matching `build/libndn-svs.so`, using the system
GCC/Boost 1.71 toolchain. The configure-time API closure passed, `ldd` bound
the integration binary to `/home/tianxing/NDN/ndn-svs/build/libndn-svs.so.0.1.0`,
and `nm` confirmed that the linked library exports
`subscribeToProducerWithCatchUp`.

That check exposed the next real defect: `ServiceProvider::fetchCollaborationDataV1Segments`
still called the portable `subscribeToProducer()` API. Its prefetch flag does
not fetch publications that were observed before the dependent Provider
installed its subscription, so the production catch-up test received no
segments and failed at `BOOST_REQUIRE(wires)` even with the correct library.

The production path now calls the bounded
`subscribeToProducerWithCatchUp()` API with the request's bounded publication
count and catch-up age. The isolated integration binary was rebuilt and the
focused `ProductionProviderContextUsesSvsSegments` test passed in three
independent process runs (RC 0 each). This is focused repair evidence, not a
new T020 source seal: the source change invalidates the previous seal and
requires a fresh G0--G3 sequence before any SIF or Tiger work.
