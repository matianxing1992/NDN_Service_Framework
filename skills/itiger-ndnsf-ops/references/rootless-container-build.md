# SIF build, delivery and storage failures

Use Slurm compute for the maintained native Waf/Apptainer build, bulk hashing
and model processing. The Tiger candidate in this workflow uses Apptainer
1.5.3 with its explicit version guard. For a different target use that target's
verified toolchain contract.

1. Seal the actual source and dependency base, definition, dependency manifest
   and installed SDK identity. Build inside the candidate boundary. Keep
   external weights in the declared model store.
2. Verify required native executables, installed libraries and ELF closure, then
   run the actual installed entry points in the clean contained image.
   `BUILT_UNQUALIFIED` is the build stage of this workflow. Qualification
   needs the applicable native, CPU, GPU and independent oracle checks.
3. Preserve build records, a measured digest, and known failure output. If
   compilation completed and packing failed, check the maintained packer's
   builder-checkpoint support and sealed inputs before rebuilding. If the exact
   SIF already exists on compute or in verified scratch, recover those bytes.
4. For artifact publication, keep a checked scratch copy when project quota
   is tight. Write small prerequisite metadata first. Stage the SIF to a
   unique partial on the destination filesystem, verify SHA256 and expected
   length, then rename it into place. Read the final path, digest and any
   declared environment setting from a clean image invocation. Bind dependent
   jobs to that exact published directory and record.

## Interpret the first storage or staging error

- `EDQUOT` can occur with ample filesystem `df` and after a build finishes.
  Measure the user/project boundary separately. Recheck bounded writes with
  fsync when reclaim has propagated. Check each finalization write, including
  the receipt. If a dependent job fails before loading, read the concrete
  missing path or quota failure before touching the already verified image.
- Distinguish stage-copy delay, graph/weight verification and model execution
  using the stage receipt and Slurm step I/O/CPU. Shared I/O can slow down and
  recover during one run. Rate measurements and checkpoint lifetime determine
  whether a current independent oracle can still authenticate on time.
- Verify writable mount, filesystem type, cross-job visibility, ownership and
  free capacity on the actual compute host. `/scratch/$USER` on a login host
  may be absent on a worker. Cache large SIFs on verified storage only where
  they will be used. File cache can be charged to a Slurm memory cgroup during
  transfer. A 4.33 GB case hit 2 GiB twice and succeeded at 6 GiB with a
  streaming SSH copy, full length and SHA256 verification. Size the cgroup
  from observed behavior on the actual filesystem.
- An allocation's `sbcast` failed in the 4.33 GB case with a header-length
  error, and a multi-node `srun` stdin retry OOMed. Stop a failed transfer
  through its own job/step and verify terminal state before cleaning partials.
  Prefer the authenticated SSH/rsync path already exercised for sealed
  delivery or the recovered shared source. Keep the maintained stage lock,
  exact hash check and native launch path. Use a cache location override
  only when the invoked launcher explicitly supports it.
- For a corrected mount, host reference or supported model/layout config,
  reuse the exact candidate when its native bits have not changed. A
  code/runtime change requires a new sealed application and requalification
  of the affected gates.
