# T022-D G3 host/CPU MiniNDN closure (2026-08-25)

> **Historical source-subject closure.** The later current-source attempt set is
> recorded in `t022-g3-current-20260825n.md`. Its retained M09 signal exit
> reopens G3 promotion; this earlier 30/30 record must not unlock a new SIF.

The current source was exercised before any SIF build in the frozen eight-node
MiniNDN topology: one controller, repository, user, router, and four distinct
Providers; 100 Mbit/s links, 10 ms one-way delay, bounded 1000-packet queues,
real NFD/SVS/ABE, tiny four-role ONNX fixture, targeted prefetch disabled, and
admission control disabled.

The accepted matrix is sealed by
`results/spec175/g3/host-minindn-manifest-20260825.json`:

| Cases | Processes | Result |
|---|---:|---|
| M01--M10, three independent processes per case | 30 | 30/30 PASS |

The accepted directories are `replay1-M01` through `replay1-M06` and
`replay2-M07` through `replay2-M10`, each with `r1`, `r2`, and `r3`.
Each case result verifies four Providers, `tiny-onnx`, disabled admission,
the frozen topology hash, zero user return code, expected terminal behavior,
artifact hashes, and clean MiniNDN teardown. M10 additionally verifies the
rotated ACK-driven role map `[1, 2, 3, 0]` and physical-artifact indexing.

The manifest records packet-lineage marker counts, bounded retry/queue policy,
source-seal and fixture digests, and process evidence. A failed M07 preparation
attempt (`replay1-M07-r1-20260825`) is retained as setup-only evidence because
Provider/2 artifact fetch exceeded the 120-second preparation bound before
Selection; it is excluded from the 30/30 matrix and does not change the
streamed deadlines or retry budgets.

This closes T022 and G3 only. It does not qualify a SIF, Qwen3.6-27B stateful
CUDA ONNX execution, or TigerCluster submission; those remain ordered after
current-source G0/G1/G2 regeneration and one final local SIF replay.
