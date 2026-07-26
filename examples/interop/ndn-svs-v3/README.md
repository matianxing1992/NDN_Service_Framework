# NDN-SVS V3 C++/NDNts Interoperability Example

This NDNSF-owned example validates an external C++ consumer of NDN-SVS against
an independently implemented NDNts peer written as actual TypeScript
(`ndnts/svs3-peer.ts`). It is intentionally not part of the NDN-SVS source
repository or its unit-test build.

```bash
cd examples/interop/ndn-svs-v3/ndnts
npm ci --ignore-scripts
cd ..
NDN_SVS_SOURCE=/home/tianxing/NDN/ndn-svs ./build-cpp-peer.sh
OUTPUT_DIR=/tmp/ndnsf-svs-v3-standalone ./run-standalone.sh
```

The standalone runner covers C++ to TypeScript, TypeScript to C++, concurrent
V3, explicit V2, and V2/V3 isolation. The MiniNDN launcher is
`Experiments/NDN_SVS_V3_Interop_Minindn.py`.

## SVS PubSub application payload mode

Spec 117 adds a separate SVS-PS gate. Both real implementations publish UTF-8
text, an opaque binary payload containing zero and non-UTF-8 bytes, 4 KiB of
deterministic data, and a 32 KiB segmented object. The independent oracle
accepts only one exact application name, byte length, and SHA-256 receipt in
each direction; StateVector convergence alone is diagnostic.

```bash
python3 tests/python/test_spec117_svs_pubsub_interop.py
python3 examples/interop/ndn-svs-v3/run-payload-standalone.py \
  --output results/spec117-svs-pubsub-payload-standalone-$(date +%Y%m%d_%H%M%S)
```

The 2026-07-16 standalone measurement is `INTEROP_INCOMPATIBLE`: 0/8 exact
payload receipts completed, although both directions observed V3 StateVector
updates. Both directions stopped at Mapping. The local C++ implementation puts
bootstrap time into its Mapping query and MappingEntry session key, whereas
the pinned NDNts SVS-PS implementation uses the specified sequence-only
Mapping form. This directory does not translate between them.

The MiniNDN launcher consumes the standalone summary and fail-closes before
importing or starting MiniNDN when that gate is negative:

```bash
python3 Experiments/NDN_SVS_PubSub_Interop_Minindn.py \
  --standalone-result results/spec117-svs-pubsub-payload-standalone-20260716_174956 \
  --output results/spec117-svs-pubsub-minindn-gate-$(date +%Y%m%d_%H%M%S) \
  --loss both
```

Current result: `NOT_ADMITTED`; neither the 0% nor 5% MiniNDN cell was started.
