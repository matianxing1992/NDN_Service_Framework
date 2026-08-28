# Fixed-vector and wire MVP evidence

Date: 2026-07-16

## Independent oracle

`tests/fixtures/svs-v3/generate-fixtures.py` is a standalone TLV encoder and
does not import or include the NDN-SVS production codec. Regeneration was
byte-stable. Important SHA-256 values:

- V2 legacy StateVector parameters: `2c598ee370f2362c21495bc40de9d8e802c0a7b8cb5530677ae6039a36aa1382`
- V3 empty: `dd12ad4676567c09e5a3c129c040bdb23c958c76b243c87488f745ef4217057e`
- V3 one node: `d051e2cc49af667f478ab91304fa808ca7cd837bef85e6bd8a089ad32c2a012a`
- V3 multi-epoch: `773fe0b325fc353c3f4ef219d3f0e021ac616c1776fc9ceef74bcd9c7996232b`
- V3 unknown extension: `af2e3b8ab7cfa01f6e189b30d9a9461c3e67f448193bf2f9d4b1a7ce5eb21aa1`

The malformed directory covers wrong Data name, missing/corrupt signature,
raw V3 StateVector, malformed Content, zero sequence number, future bootstrap,
duplicate core object, unknown core TLV, and truncated extension input.

## Focused execution

```bash
cd /home/tianxing/NDN/ndn-svs
./waf -j4
LD_LIBRARY_PATH=$PWD/build ./build/unit-tests --run_test=TestV3Wire
```

Result: 7/7 V3 wire tests passed. Production V3 parameters matched the
independent one-node fixture byte-for-byte. The decoder accepted all positive
fixtures and rejected the structural/semantic negative set.

The initial V2 fixture was corrected after independent NDNts interoperation
showed that upstream V2 entries use `Name + SeqNo(TLV 0xCC)`, not the V3
bootstrap tuple. The corrected fixture and the isolated V2 codec now match the
independent peer byte contract.

Additional focused tests passed individually: default V3 envelope and 1000 ms
lifetime; one V3 receive advancing exactly one callback range; V3 route
rejecting V2 without mutation; parallel V3 production through the shared
codec; explicit V2 propagation through `SVSPubSub`; and zero sequence rejection.

The build-library path was explicitly bound through `LD_LIBRARY_PATH`; without
it, the executable loaded the older `/usr/local/lib/libndn-svs.so`.
