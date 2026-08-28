> Draft for the local Boost 1.74 candidate `ee4e174`. PR #36 remains on
> `0c09d65`; apply this text only after the user explicitly authorizes another
> push and the exact candidate is published.

This PR adds SVS v3 support while keeping SVS v2 available through an explicit
protocol selection. It also contains the existing asynchronous PubSub work and
the fork's Mapping/Repair reliability extensions.

## SVS v3 behavior

Commit `9f679bd` is the V3 implementation. It provides:

- the `/v=3` Sync Interest name;
- a signed StateVector Data packet inside ApplicationParameters;
- ParametersSha256DigestComponent handling;
- validation of the complete packet before state is updated;
- BootstrapTime-aware state vectors;
- version-specific local Interest filters with group-prefix NFD registration;
- deterministic valid and invalid wire fixtures;
- explicit V2/V3 isolation.

The group-prefix registration change is important for interoperability: the
C++ and NDNts peers can now discover each other through NFD without manually
injecting routes.

## Commit order

1. `815bd62` - regex subscriptions and name-only publishing
2. `3b709b4` - bounded Mapping/Data piggyback delivery
3. `cfc6270` - parallel Sync receive, local batching, and timer control
4. `e7cd98e` - parallel production and ordered async PubSub publishing
5. `ee9632d` - V2 signed-Interest encoding through `InterestSigner`
6. `9f679bd` - complete SVS v3 implementation and protocol tests
7. `c767560` - sparse Mapping recovery without duplicate fetches
8. `79d830a` - failure-atomic segmented publication
9. `ee4e174` - bounded segmented fetch, Repair, and atomic extensions

Commits 7-9 are fork reliability work. They do not alter the official V3 wire
envelope owned by `9f679bd`. Every reviewed commit keeps Boost 1.74; local
Boost 1.71 compilation is performed only on deleted-after-use `compileTMP`
branches and is not part of this PR.

## Validation

The local exact head `ee4e174` passed:

- all 71 C++ unit tests locally, with every one of the nine exact OIDs also
  passing its complete available suite;
- five standalone C++/NDNts TypeScript cases: both V3 directions,
  concurrent bidirectional V3, explicit V2, and V2/V3 mismatch isolation;
- six MiniNDN cells, run once each: three at 0% loss and three at 5% loss,
  with 20 publications per peer.

The MiniNDN result was 240/240 bidirectional observations, equal final vectors
in all six cells, and zero duplicates, rejects, peer restarts, or Sync-Ack
packets. The executable TypeScript/C++ interoperability harness is kept in the
NDNSF repository rather than added to this library; this PR retains only C++
code and unit fixtures.

GitHub Actions for this exact head remain the final publication gate and are
not available until the user authorizes publication.

## Recovery

The previous PR head is preserved at
`matianxing1992/ndn-svs:safety/spec115-pre-final-publication-20260717T004458Z`
(`2b052c9`). Restoring it requires an exact force-with-lease from `0c09d65`;
the backup branch is not deleted by this update.

This PR does not claim an upstream merge or release.
