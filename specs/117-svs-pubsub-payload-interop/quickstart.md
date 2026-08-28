# Quickstart: SVS PubSub Payload Interoperability

## Preconditions

```bash
cd /home/tianxing/NDN/ndn-service-framework
test -f /home/tianxing/NDN/ndn-svs/build/libndn-svs.so
test -x /home/tianxing/.local/node-v22.23.1/bin/node
```

## Build and Focused Checks

```bash
examples/interop/ndn-svs-v3/build-cpp-peer.sh
cd examples/interop/ndn-svs-v3/ndnts
npm ci --ignore-scripts
node_modules/.bin/tsc --noEmit --target es2022 --module nodenext \
  --moduleResolution nodenext --skipLibCheck svs3-peer.ts
cd /home/tianxing/NDN/ndn-service-framework
python3 tests/python/test_spec117_svs_pubsub_interop.py
```

## Standalone Gate

```bash
python3 examples/interop/ndn-svs-v3/run-payload-standalone.py \
  --output results/spec117-standalone-$(date +%Y%m%d_%H%M%S)
```

Expected positive result: eight unique receive receipts, four in each
direction, exact name/length/SHA-256 matches, and multiple segments for both
segmented-direction receipts.

If the command reports `INTEROP_INCOMPATIBLE`, preserve the directory and stop.
Do not run MiniNDN or add a compatibility shim.

## MiniNDN Gate

The launcher itself enforces the standalone gate. A negative standalone result
writes a `NOT_ADMITTED` receipt for both cells and imports/starts no MiniNDN
runtime:

```bash
python3 Experiments/NDN_SVS_PubSub_Interop_Minindn.py \
  --standalone-result results/spec117-standalone-<timestamp> \
  --output results/spec117-minindn-<timestamp> --loss both
```

After standalone success, run the same command through `sudo -n`; each admitted
cell contains the corpus manifest, peer event streams, summary JSON,
stdout/stderr, normal producer-registration RIB evidence, and packet captures.
