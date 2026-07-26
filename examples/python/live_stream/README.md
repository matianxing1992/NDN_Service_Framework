# Generic LiveStream example

The provider reserves meaningful semantic Data names before publication and
hands only opaque bytes to NDNSF. The consumer opens a validated descriptor,
keeps future exact-name Interests outstanding through the Core handle, and
receives Provider-authenticated opaque bytes. No key, cipher, UAV type, manual
Mapping route, or manual Face scheduling appears in either application.

Run the MiniNDN regressions instead of host NFD. The first starts one
`Beginning` and one delayed `Latest` consumer from immutable descriptor
snapshots; the second enables `reserveGroup`/`publishGroup` FEC over opaque
bytes at 5% configured link loss:

```bash
sudo -n -E python3 Experiments/NDNSF_LiveStream_Minindn.py \
  --loss 0 --count 12 --consumers 2 \
  --output results/spec119-live-stream-minindn-dual-current

sudo -n -E python3 Experiments/NDNSF_LiveStream_Minindn.py \
  --loss 5 --count 12 --start beginning --fec \
  --output results/spec119-live-stream-minindn-fec-loss05-current
```
