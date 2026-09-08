# Spec183 base-SIF read integrity investigation

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
