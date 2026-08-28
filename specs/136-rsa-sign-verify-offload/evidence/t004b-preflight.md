# Spec 136 T004b Preflight Evidence

**Date**: 2026-07-23  
**Verdict**: PASS  
**Scope**: Admission, security, caller ownership, and provenance only. This is
not formal performance evidence.

## Frozen Subject

| Artifact | SHA-256 |
|---|---|
| R6 benchmark binary | `f6dd0038bddd3eb7f2803a83dda26205b8d5878da3d68cffdde55e22fb70ef27` |
| NDN-SVS library | `7945f22bcdaaef149f4e3cc2a75a39d124e4dfd54d07027cadcc83b4f5b1308f` |
| Build manifest | `2f90fd74f098a4f1296ed9d3c69f14f703f060192c816ec7cd7753cd278410f9` |
| Preflight summary | `ce4fa780fc9492472284d18cfd7feeeb232ede9d34ec04d8edebaefa15d11b35` |
| Sealed formal manifest | `c1096bcac07ac177b6e4fd6eb89a0a08feb75b65f8291f3e92415821a5d55c32` |
| Formal runner | `366052f0da7ca8544c67fbda444a27e4cf8f7e16c635b76afa211c15ebef7d98` |

The build manifest records GCC 9.4, Boost 1.71 linkage, NDN-SVS base commit
`6bb34545b4f89f1f6c265a68c18f1a40ade413eb`, all modified NDN-SVS paths,
source hashes, build command, linkage, and effective CPU affinity `0-3`.

## Independent RSA Tamper Probes

| Peer | Data type | Interest type | Valid Data | Valid Interest | Tampered Data | Tampered Interest | Tampered processing |
|---|---:|---:|---|---|---|---|---|
| peer-a | 1 (RSA) | 1 (RSA) | accepted | accepted | rejected | rejected | 0 |
| peer-b | 1 (RSA) | 1 (RSA) | accepted | accepted | rejected | rejected | 0 |

Each peer used its persistent file PIB/TPM identity. The negative probes
changed signed content/name material after signing and passed through the same
fixed-certificate validator implementation used by the benchmark.

## Two-Peer 1000 pps No-Op Pacer

Each row is a 60-second measured window. These processes perform no RSA
signing, NDN publication, Sync Interest, or Fetch operation.

| Mode | Peer | Scheduled | Attempted | Attempted pps | Calls on io_context | Calls on APP pacer |
|---|---|---:|---:|---:|---:|---:|
| face-inline-rsa | peer-a | 60,000 | 60,000 | 1000.00 | 60,000 | 0 |
| face-inline-rsa | peer-b | 60,000 | 60,000 | 1000.00 | 60,000 | 0 |
| worker-rsa | peer-a | 60,000 | 60,000 | 1000.00 | 0 | 60,000 |
| worker-rsa | peer-b | 60,000 | 60,000 | 1000.00 | 0 | 60,000 |

All four summaries prove distinct io_context and APP-pacer thread IDs. This
closes SC-007 without confusing RSA/NDN capacity with pacer capacity.

## Fresh Bidirectional MiniNDN Smoke

| Mode | Peer | Attempted | Unique delivered | Delivery | Data valid | Interest valid | Invalid | Outstanding | Abandoned |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| face-inline-rsa | peer-a | 600 | 600 | 100% | 1,604 | 1,217 | 0 | 0 | 0 |
| face-inline-rsa | peer-b | 600 | 600 | 100% | 1,628 | 1,217 | 0 | 0 | 0 |
| worker-rsa | peer-a | 600 | 600 | 100% | 1,602 | 1,479 | 0 | 0 | 0 |
| worker-rsa | peer-b | 600 | 600 | 100% | 1,600 | 1,479 | 0 | 0 | 0 |

Both cells used two MiniNDN nodes, bidirectional exact-remote-prefix
subscriptions, 200 pps per peer, real RSA Data/Interest signing and peer
validation, worker count `0|1`, the required caller ownership, and one active
serialized signer.

## Sealing And Negative Control

- The sealed manifest contains exactly ten cells at
  200/250/300/350/400 pps per peer in the registered alternating order.
- The manifest hashes the R6 build and this preflight summary and protects the
  canonical R3 400-pps confirmation tree.
- Formal execution requires an explicit `--formal --sealed-manifest ...`
  command. T004b did not invoke it.
- The canonical R3 confirmation tree at R6 sealing remains
  `66fe4f787f258e6f924141b0ae706f5776dc06af2d04098443bc7d09f409f7b6`.
- R4 is retained as superseded evidence. Its preflight passed, but its sealed
  manifest incorrectly made future explicit execution impossible; no formal
  cell ran under R4.
- R5 is retained as superseded evidence. Its preflight passed, but its formal
  loop could stop after a non-`COMPLETE` cell and thereby omit later registered
  cells. R6 removes only this fail-fast behavior, then repeats the entire
  preflight before sealing. No formal cell ran under R5.
