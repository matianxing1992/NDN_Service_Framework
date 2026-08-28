# Standalone independent-peer interoperability

## Spec 115 rewritten-history candidate

Both the final replacement owner `218ad952...` and rewritten Experimental
`fd99b3f...` passed the five-case standalone C++/NDNts matrix: each directional
case delivered 5, concurrent delivered 5 each way, explicit V2 delivered 5 each
way, profile mismatch delivered 0/0, and SyncAck remained 0. The canonical
successful summary digest is
`43a7c166ee1e6e25ecafeca23e532f608b45307009a0555f0c2ce74c9f30e7a1`.

One initial Experimental host-NFD attempt passed two cases and then hit an
on-demand face removal before route installation. The partial result was
preserved; a clean-NFD new-directory retry passed all cases. No formal MiniNDN
cell was rerun.

Date: 2026-07-16  
Source: `Experimental@53dd1588201b967a4aa9decd3e51ade3263e0f88`

NDNts `@ndn/svs@0.0.20250307` was installed from lockfile SHA-256
`ef41e08e787c74c9fc424c96684f8c163a017ffd1a3b82bc064bf75df7ffd95c`
and executed with Node.js `v22.23.1` under the shared deterministic HMAC test
policy.

```bash
cd /home/tianxing/NDN/ndn-svs
tests/interop/run-svs3-interop.sh --output /tmp/spec114-standalone-converged2
```

| Case | C++ observed | NDNts observed | Result |
|---|---:|---:|---|
| C++ to NDNts | 0 | 5 | PASS |
| NDNts to C++ | 5 | 0 | PASS |
| concurrent V3 | 5 | 5 | PASS |
| explicit V2 | 5 | 5 | PASS |
| V2/V3 mismatch | 0 | 0 | PASS |

The first post-audit harness attempt exposed a transient-face selection race:
a short-lived `nfdc` management face survived two 50 ms snapshots and vanished
before route installation. That diagnostic directory is preserved. The harness
now requires peer-sized protocol traffic before selecting a face; the fresh
5/5 run passed and its temporary host NFD was cleaned up.

- aggregate summary SHA-256:
  `43a7c166ee1e6e25ecafeca23e532f608b45307009a0555f0c2ce74c9f30e7a1`
- concurrent C++ / NDNts events: `8a8fea1f...5ce9` / `102b41f3...e8cd`
- explicit-V2 C++ / NDNts events: `d6fe736a...400d` / `fd769300...bebb`
- peer rejects and Sync Ack Data: zero
