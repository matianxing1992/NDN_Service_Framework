# Spec170 TigerCluster Lessons

These are reusable operating rules distilled from the recent NDNSF-DI
Spec170 qualification work. They describe development-candidate handling and
must not be read as a release PASS. The repository's active Spec Kit ledger
and candidate-bound evidence remain authoritative.

## Normal local-SIF promotion

- The normal release starts with a locally built SIF, which may be uploaded directly to
  `/project/$USER/ndnsf-di/releases/<candidate-id>/runtime.sif`. This removes
  remote image materialization and cluster-side rebuilding. It does not remove
  source sealing, candidate identity, hash verification, runtime inspection, or
  Slurm workload gates.
- Build the SIF on local node scratch or `/tmp` after a writable-capacity
  check. Record the source revision, definition hash, Apptainer version, byte
  count, and local SHA-256 before transfer. Never overwrite an existing
  candidate directory.
- An unprivileged `apptainer build --userns` can fail because the local host
  lacks `newuidmap`; classify that as a builder/UID-mapping limitation, not a
  runtime or image failure. Use only an approved local privileged builder path
  and retain the failed user-namespace attempt.
- For multi-gigabyte transfers, use a resumable or otherwise bounded method,
  monitor the transfer process, and do not submit a job while the remote file
  is partial. Verify remote byte count and SHA-256 independently; only then
  run `apptainer inspect` and submit a gate with the exact SIF path plus
  `SPEC170_RUNTIME_SIF_SHA256`.
- Keep v3/frozen and v4/overlay candidates in different immutable directories.
  An overlay that adds a development executable is useful for a bounded gate,
  but is not the frozen T024 release until the source and build closure are
  sealed again.

## Historical Docker/GHCR cleanup boundaries

These notes describe old diagnostic routes only. They are not alternate
release instructions: do not start a current Spec170 release with GHCR,
`apptainer pull docker://...`, or a Tiger-side builder when a local SIF can be
built.

- A Docker or GHCR TLS/layer failure is evidence about the transfer/build
  route, not proof that the source or SIF runtime is invalid. Preserve the
  first failure and do not blindly retry the same release identity.
- A local SIF is the current Spec170 release artifact. Keep the source seal,
  local build record, SIF digest, and reason for any route change together;
  do not infer a network or GPU PASS from the artifact alone.
- Before cleanup, enumerate exact superseded SIF/cache paths and their sizes.
  Remove only those paths; preserve the active SIF, release manifests, logs,
  and evidence. Verify free space and a small write probe afterward. A stated
  `/tmp` or scratch retention window is not durable evidence storage.

## Python/native preflight boundary

- A green C++ unit/integration binary does not prove that the Python runner can
  load the same candidate. The binaries may link directly to freshly built
  objects while `pythonWrapper/ndnsf/_ndnsf*.so` still resolves an older
  `/usr/local/lib/libndn-service-framework.so`.
- Before every Tiger submission, build the shared framework library, rebuild
  the extension for the active Python ABI with an explicit candidate library
  directory, and require both `python3 -c 'import ndnsf._ndnsf'` and the real
  runner's `--help` command to succeed. Record extension/native-library hashes,
  RPATH/RUNPATH, `ldd` closure, and the exact import/help output.
- An unresolved C++ symbol such as
  `DeploymentControlMessage` is a local candidate-closure failure. Stop before
  SIF promotion/Tiger submission; do not classify it as a MiniNDN or cluster
  runtime failure and do not fix it by mounting an unrelated replacement `.so`.

## SIF internal-library closure is a hard gate

- Import success is necessary but insufficient. A candidate can import while
  carrying build-host RPATH entries, stale SONAME links, or a different
  NDN-CXX/NDN-SVS/NAC-ABE/Boost/ONNX Runtime library family than the Provider.
- Run `scripts/validate-sif-library-closure.py` against the exact SIF,
  manifest-selected Provider, active Python extension, and every packaged
  library root, with the sealed library lock supplied via `--lock`. Keep its
  JSON inventory beside the candidate build record. The result must be `PASS`
  before upload and again after Tiger staging; a missing lock is a gate
  failure, not permission to run an inventory-only check.
- The gate rejects unresolved `ldd` entries, `/tmp` or `/home` RPATH/RUNPATH,
  missing versioned SONAME/compatibility links, duplicate library families,
  and hash/SONAME mismatches against a supplied `ndnsf-sif-library-lock-v2`.
  Every locked library
  row must also carry a non-empty package/toolchain `version`; a hash-only row
  is not a version-consistency check. SONAME links are matched within their
  own packaged library root, never against a same-named file in another root.
  Internal NDNSF/NDN-SVS/NDN-CXX/NAC-ABE/OpenABE/RELIC/ONNX Runtime libraries
  may not fall back to `/lib` or `/usr/lib`; external Boost is allowed only
  when its SONAME major/minor matches the lock.
  It may bind a host
  `readelf` solely to inspect ELF metadata when the image lacks binutils; that
  tool is not a runtime dependency and cannot make an incomplete SIF pass.
- The checker also runs `ldd` for every distinct packaged `.so` payload,
  rejects libraries present in the SIF but absent from the sealed lock, requires
  a non-empty toolchain/version map, and rejects absolute RPATH/RUNPATH or
  compatibility symlinks that escape the recorded roots.
- If this gate reports a host build path or a version mismatch, rebuild the
  extension/Provider with release-only RPATHs and a fresh release identity.
  Do not silence the row because `python import`, a unit test, or a previous
  node happened to work.

## Apptainer environment and GPU checks

- `--cleanenv` can discard scheduler/device variables. The container launcher
  must explicitly pass the values needed by the workload (at minimum the
  allocated GPU mask and candidate directory), and the workload must print
  and validate them inside the container.
- GPU gates require `apptainer exec --nv`, an observed
  `CUDAExecutionProvider`, exact visible-device mapping, real model execution,
  and `cpuFallbackUsed=false`. A static import or host `nvidia-smi` is not a
  GPU PASS. CPU/no-GPU D0 is a separate gate.
- A valid SIF does not prove NDNSF-DI service behavior. A candidate-bound
  network gate still needs Controller/User/Provider status, permissions,
  `REQUEST -> ACK -> ACK_CLOSED -> SELECTION -> RESPONSE`, and independent
  exit-status validation for every process.
- Run the semantic contract preflight with the exact plan and matching driver
  pair. A `DATA_DRIVEN_V2` plan paired with a legacy or unrelated cross-Provider
  wrapper is a preflight failure; inspect the plan policy before submission and
  retain the mismatch as negative evidence rather than changing a timeout.

## Build and harness closure

- Build the complete Waf example target census, not only the first smoke
  target. Sibling ONNX and Provider targets must each declare the full
  `BOOST NDN_CXX NDN_SVS ONNXRUNTIME DL` closure; fixing one target does not
  repair another. Retain the first undefined-reference log if any target
  fails.
- Stage every Slurm wrapper and helper together, verify paths and hashes, run
  `bash -n`, and invoke staged scripts explicitly with `bash`. A trailing
  `echo`, cleanup command, or aggregate `grep` must not replace a failed child
  status with PASS.

## D2a evidence boundary

- Seeing two GPUs and loading real ONNX Head/Shard roles on `cuda:0` and
  `cuda:1`, followed by an authenticated local collective and a rejected
  wrong-proof negative, is useful model-backed smoke evidence.
- It is still only partial T032 until the sealed `DEVICE_SET`/fencing
  lifecycle, full ProviderRoleWorker 3A graph, unsplit-stage oracle, and the
  specified 50 fixed race seeds are exercised. Adapter-neutral collective
  smoke and two independent Providers are not substitutes for one Provider
  with a local two-GPU role.
- Do not start D2h from partial D2a evidence. D2h requires independently
  closed D2a and D2b gates; retain BLOCK rather than widening the claim.

## Evidence discipline

- Promote manifests, hashes, Slurm output, allocation/GPU metadata, and
  negative evidence to project storage before the allocation ends. Treat
  `/tmp`, node scratch, and local build directories as disposable.
- Record exact candidate identity in every job and result. A successful later
  diagnostic cannot overwrite an earlier failed identity or silently turn a
  development overlay into a frozen release.

## Final staging-layout preflight

- `seal-rootless-source.py create` validates the source bundle it writes, not
  the directory after upload/copy. The rootless wrapper later reads dependency
  archives from `.spec110-build/archives`, so checking only the root
  `archives/` directory is insufficient.
- Before every Slurm build, run
  `scripts/validate-sealed-source-root.py` on the exact local staging root and
  again on the exact remote staging root. It verifies the seal body digest,
  workspace size/digest, lock digest, mirrored manifest, and byte-identical
  dependency archives at both paths. Retain both compact JSON reports beside
  the job manifest.
- A missing mirror is a pre-dispatch failure, not a builder failure. Do not
  patch a running allocation or reuse the failed release identity after
  correcting staging; create a new release identity and rerun the preflight.
- A SIF produced before a trailing cleanup error remains artifact-ready but
  the Slurm job is orchestration-failed. Verify the exact SIF in a separate
  bounded job instead of rebuilding the immutable image for a wrapper-only
  defect.
- Before D1/D2, run the exact-SIF `allocated-gpu` probe with the same `--nv`
  bind map and hash as the network gate. On 2026-08-16, the probe on
  `itiger07` consumed only 3 CPU seconds while reading about 984 MB in 4:53
  without producing JSON; it was cancelled and the node/path was marked
  unqualified. Do not submit a Provider workload on a launch path that has
  not closed this preflight.
- Treat the D1 workload bundle as a candidate-bound artifact, not as a
  reusable wrapper. On 2026-08-16, the D1 bundle reused the same execution
  plan as the passing D0 bundle but had different SHA-256 values for
  `service-manifest.json` and `user_driver.py`. CUDA Provider READY evidence
  therefore passed while the request later carried an invalid
  `/Inference/NativeTracer` assignment. Compare plan, manifest, user driver,
  policy/trust files, and referenced artifact hashes before allocation; reject
  any mismatch and generate a fresh workload from the current candidate.
- Use `scripts/validate-spec170-network-bundle.py` for that comparison and
  retain its JSON output beside the Slurm manifest. A non-zero result blocks
  dispatch; a prose checklist alone is not an enforced preflight.
- Relative artifact names require an explicit Provider working-directory
  contract. Job 197169 reached CUDA but failed to load the first model because
  the workload launched outside the staged bundle. Require `cd "$BUNDLE"`
  (or equivalent absolute artifact resolution) and a pre-load file check.
