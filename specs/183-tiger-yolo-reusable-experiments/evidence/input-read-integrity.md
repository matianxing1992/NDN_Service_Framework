# Spec183 base-SIF read integrity investigation

## 2026-09-08 follow-up

After earlier same-day matching reads, the new base renderer again rejected the
local image. Two ordinary reads and a sudo read returned
`a2600783605752df995ec002f9eab915f35167196f9fc2e1cf62e6de6bd64e68`.
A subsequent `dd iflag=direct bs=4M | sha256sum` returned the pinned
`b6710fd696a7f962f67f67a54278d92a15babb856c4af428a3f04ba54dc83285`.
Evidence: `results/yolo-layered-20260908/preflight/base-direct-read-sha256.txt`.
This isolates a buffered/direct read discrepancy; it does not prove its kernel,
hypervisor or physical-memory root cause. Sudo alone does not repair the issue.
No matching kernel I/O/ext4/hardware/OOM messages appeared in the preceding
30-minute bounded search. Request file-specific `POSIX_FADV_DONTNEED`, then verify
ordinary reads before continuing; no global cache flush or lock change.

The file-specific eviction completed, and the next ordinary SHA256 read returned
the pinned b6710fd6 digest (`base-after-fadvise-sha256.txt`). A separate renderer
read also passed (`base-render.json`), allowing the local base build to start.
This is a successful bounded recovery of the current input check, not proof of
a permanent host repair.

The next extraction nevertheless failed on `libcudnn_adv.so.9.1.0` with a gzip
error (`base-build-2.log`). File-specific cache eviction alone is therefore not
an adequate build workaround. A direct-I/O copy to the owned tmpfs file
`/dev/shm/spec183-base-20260908/base.sif` passed the same locked digest check
(`base-render-3.json`) and the third build successfully extracted it, reaching
APT. The temporary copy is not a new release identity. Keep it while build FDs
remain open and remove it after completion. Native compilation and final image
verification remain separate checkpoints.

## Historical observations

2026-09-07. Status: UNRESOLVED. No hash/lock or canonical cache link was replaced.
No runtime or GPU job was submitted. This is a historical base input, not the
Spec183 final SIF.

1. Independent local Python and system sha256sum reads of the original inode
   4731540 produced `6a3d001088305a9e189c7e97fe1ed19c8167347341de1ca23a1e67076f564b94`.
   Canonical base and inputs paths were hard links to that same inode.
2. Live read-only SSH to the retained source
   `/project/tma1/ndnsf-di/candidates/spec180-runtime-b6710fd6/spec180-runtime.sif`
   returned pinned `b6710fd696a7f962f67f67a54278d92a15babb856c4af428a3f04ba54dc83285`.
3. A slow full scp was stopped by its confirmed PID 1404729. It was replaced by
   rsync --checksum --no-whole-file --copy-dest to a distinct recovery directory.
   The original stayed intact. Rsync exit 0: 59,376 literal bytes, 3,525,802,000
   matched bytes, 297,035 network bytes received. Recovered size 3,525,861,376.
4. System sha256sum initially verified the recovery file as b6710fd6. cmp reported
   the first difference from the original at byte 1,092,472,020. The replacement
   routine rehashed before touching any canonical link, failed its assertion,
   and therefore performed no quarantine/link replacement.
5. A later read of a 65,536-byte window starting at that differing byte returned
   identical bytes from both files, SHA-256
   `321e9f13e22239c717965b58f7457c72f35b873781413791c742c30edc9a6845`.
6. One subsequent complete read of the recovery file fed identical chunks to
   OpenSSL hashlib and CPython's independent _sha256 implementation. Both returned
   `c01cbda15b14f908edf8be8befb9f653626e71c0903df3908d115d53216815be`, not the earlier
   b6710fd6. File size/inode/mtime/ctime were stable within that 43.71-second read.

These results do not support a stable-byte-corruption diagnosis or an OpenSSL-only
hash failure. Host/storage/read-path instability remains possible; its cause is
not established. Do not change release identities to a surprising observation,
retry model execution, repeatedly download the image, or claim a cache repair.
Preserve original and recovery files under the ignored base-sif cache for bounded
host-level investigation. The replacement guard correctly prevented promotion.
The local mount is /dev/sda5 ext4 (rw,relatime,errors=remount-ro). A bounded kernel
journal search over the preceding 30 minutes found no matching I/O, ext4, machine
check, hardware-memory or OOM errors; absence of such logs does not prove healthy
reads. No host setting, mount option or memory setting was changed.
# 2026-09-08: one cached byte differs from direct I/O and RAM

Retained logs under `results/yolo-layered-20260908/preflight/` now locate a
specific difference in the new d4031191 base, rather than only differing hashes.
`page-cache-difference.log`: file offset3188006099, buffered byte0xba versus
RAM/direct-I/O byte0xbb; one differing byte in its4MiB block. Direct I/O matches
RAM and disagrees with buffered reads. `disk-ram-joint-hash.log` reports disk
d9d255f3… versus RAM d4031191…, while coreutils independently confirms RAM d403.
Inode3802628, size3900682240 and mtime remain unchanged.

Invalidating only the4096-byte containing page restores0xbb and the complete
d403 digest (`target-page-invalidation.log`), but the following input-render
attempt reproduces d9d255f3. This is not a permanent repair. The evidence locates
a cached-read bit difference; hardware, virtualization or kernel cause remains
unproven. Stop repeated disk-side retries and do not rewrite the locked digest.

Current workaround: keep `/dev/shm/spec183-sdk-d4031191/base-runtime.sif`, whose
bytes retain d403, and hard-link it into RAM-resident I/R planes. Only small
metadata (at most4MiB) may copy across filesystems; SIFs are never copied by that
fallback. The owned RAM snapshot is now user-owned0444 so Linux protected
hardlinks permits the task's links; no system-wide hardlink protection changed.
Actual production content check passes using RAM, while runtime qualification
remains NOT_EVALUATED. Durable metadata archive excludes SIF payloads.
