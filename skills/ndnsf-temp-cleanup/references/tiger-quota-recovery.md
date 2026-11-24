# Tiger shared quota and experiment cache recovery

## Locate the boundary

An `EDQUOT` write or fsync gives direct evidence about that file's quota
boundary. Compare it with the actual mounts and ownership on the host that
failed. Global `df`, an application cache limit and local disk capacity answer
different questions. Treat unknown per-user/project headroom as unknown.

Slurm staging can run slowly while bytes continue advancing. Check stage
receipts, read/write counters and the exact failure. `OUT_OF_MEMORY` during a
file copy can mean that the job's memory cgroup charged its file cache. Check
the actual compute node's mounts and cgroup and the copy's partial size.
A login host's scratch directory may have different ownership and visibility.

## Reclaim by exact ownership

1. Check active and pending jobs, stage locks, mounts, owner and inode links.
   Keep the current qualified SIF, model/Repo objects and any distinct frozen
   comparison input needed for an active or planned run.
2. Select only specifically authorized idle cache, superseded copies and
   abandoned partials. Consider generated provider outputs and node-local
   caches independently from the uniquely retained source and historical
   result evidence. Terminal Slurm state alone is insufficient for a
   workspace with unresolved closeout.
3. When retaining an image or unique archive, stage it in a separately verified
   writable destination, read back the full SHA256 and length, then publish the
   archive. Record old path, archive path and digest. Recheck source identity
   and consumers before removing the original shared copy. If volume quota is
   tight on both paths, keep the verified copy and recover the minimum needed.
4. Remove an abandoned partial only after checking the exact job/step is
   terminal, ownership, file type, symlinks, open handles and the stage lock.
   A partial of the expected byte length still needs its own digest proof.
   Preserve the verified final image and other jobs' partials.
5. Count actual allocated bytes by unique inode. Hardlinks to frozen weights
   may look like new tens-of-GB assets even when their release returns no
   blocks. Count project-quota recovery separately from node-local space and
   storage used by a retained scratch archive.

## Verify the recovery

Recheck allowed scoped writes on the destination itself, including close/fsync.
Allow bounded backoff after reclaim for delayed quota accounting. A small probe
shows that the tested write worked at that time. For a multi-GB artifact, verify
the final production publish and all small receipt writes before releasing the
dependent jobs. Stage the new candidate in a partial, verify SHA256 and full
length, and atomically rename after the check. If a completed exact SIF is
already available in checked scratch, recover its bytes before scheduling a
rebuild.

In the November 2026 Tiger runs, two nominally complete builds were blocked
by project quota during a large copy or a late metadata write. Retiring
verified duplicate SIFs and an incomplete output restored actual project
writes. A 4.33 GB node-cache transfer later hit a 2 GiB cgroup limit. The
verified retry used 6 GiB, and the duplicate and abandoned partials were
retired after its owner no longer needed them. These values describe one
workload. Measure the next filesystem, job and candidate independently.
