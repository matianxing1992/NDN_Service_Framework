# SIF build and verification

The current delivery is a qualified dependency base SIF plus one complete NDNSF
application SIF. Use
`Experiments/TigerCluster/adapters/slurm-apptainer/scripts/build-local-sif.sh`.
The external APP packager is a historical compatibility path, not an additional
step for a complete application SIF. Models and supported deployment parameters
are external inputs; changing them alone does not require rebuilding the image.

## Apptainer version policy

Use Apptainer **1.5.3** for base and application builds, including checkpoint
repacking. Record the actual executable, version and digest on the build node.
Do not change `--expected-apptainer 1.5.3` to bypass a mismatch. On Tiger, builds,
full-file hashing and all inference run inside a Slurm compute allocation.

## Before building

- Pin the source seal, definition, base SIF digest and dependency manifest.
  Pending source changes must be included in the seal; HEAD alone is insufficient.
- Inspect the actual consumed headers, libraries, compiler and Waf configuration.
  Build application outputs inside the sealed container boundary. Do not copy
  host-built libraries or extensions into the image.
- Check the actual filesystems and writable capacity for build temporary files,
  candidate output and persistent caches. `df` does not prove user quota headroom;
  quota remains unknown if the service provides no limit. A directory named
  `/var/tmp` is not evidence of disk capacity or cross-node persistence.
- Reuse a validated base and the existing locked workspace. If compilation
  completed and final packing alone failed, inspect the maintained
  `--builder-checkpoint` contract before recompiling. A checkpoint needs its
  retention receipt, SDK digest, original definition and completed build evidence.

Run the maintained script without arguments to inspect its required options.
It rejects missing inputs, an existing output, and an incorrect Apptainer
version. `--build-only` explicitly creates **BUILT_UNQUALIFIED**, not release PASS.

## Candidate checks and their limits

The builder validates source identity and image labels, then executes the final
native verifier inside the actual candidate with a clean, contained environment.
The release path additionally invokes its image preflight before emitting the
build record. Preserve the record and final SIF digest. Inspect the actual native
output inventory, resolved libraries and runtime paths; a matching SONAME or a
successful `--help` alone does not establish ABI or inference compatibility.

These checks are necessary but do not establish CUDA model execution. Verify
the following in order, retaining their separate results:

1. Relevant native tests and a small installed MiniNDN CPU cold/warm run for
   changes shared by CPU and CUDA.
2. Admission of the exact candidate and real CUDA initialization/session
   lifecycle in a GPU allocation, including device/EP identity and no fallback.
3. Qwen 0.6B cold/warm inference with independent output/state checks and normal
   cleanup, before the expensive 27B workload.
4. The feature's required model matrix using the same final source/release/SIF.

An SDK probe, image build, model preparation and complete inference are different
results. A failure at any one boundary does not qualify subsequent boundaries.
Use native C++ runtime applications; host Python orchestration or offline model
preparation does not justify Python inference inside the runtime container.

## Retrying a failure

Use the first failed boundary to select the repair and the next check:

| First boundary | Required check before retry | Rebuild scope |
| --- | --- | --- |
| Missing SDK header/library or wrong ABI | Compile/link/run the actual consumer inside the sealed base; inspect resolved libraries | Base only when its declared dependency closure is incomplete; then application |
| Waf compile/link failure | Inspect target inputs, flags and object output names; reproduce the affected target before full packaging | Affected application targets |
| Final SIF packing or output capacity | Verify retained builder checkpoint, SDK identity, free disk and available quota | Repack the verified checkpoint; preserve completed compilation |
| Final image import, loader or native verifier | Run the exact failing consumer in the candidate's clean default environment; inspect hashes, RPATH and dependencies | Application, or base only if the failure is in its closure |
| Mount, path, credentials or host launcher | Verify effective configuration and the actual file/permission/readiness boundary | Configuration or host tool; reuse the unchanged image |
| CUDA/model session or inference | Test the exact candidate on a GPU, then 0.6B cold/warm before 27B | Only the component identified by the first failure |

Every retry must retain the previous failure and show which input or check has
changed. An unchanged retry is appropriate only for an identified transient
failure, with bounded backoff. Do not repeat a full build to discover the same
unrepaired deterministic error. A prior image's PASS does not qualify a changed
source seal, dependency base or candidate digest.

Retain the first failing command, exit status, source/configuration identity and
timing in the active feature evidence. Diagnose that boundary before repeating
the operation, and update its affected check. Do not rebuild for a corrected
mount, storage path, supported model parameter or host-only control change.
Source/runtime changes require a new application candidate; dependency changes
require the applicable approval and base validation. Repacking a checked native
checkpoint does not itself establish runtime qualification.
