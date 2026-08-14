# Spec 170 Gate C Evidence (current checkpoint)

**Verdict: PARTIAL PASS.** The latest immutable GPU OCI candidate passed its full
GitHub build, runtime probe, signature, SBOM, anonymous pull, and
release-manifest checks. It has now also been materialized to an exact SIF with
matching local/project hashes. The SIF is ready for the no-GPU static probe;
full Gate C and Tiger D0/D1/D2 completion are still unclaimed.

The current Qwen3-compatible candidate is documented in
`tiger-qwen3-sif-materialization-20260814.md`. The earlier SIF below remains a
diagnostic-only runtime and must not be mixed with this candidate.

## Latest Qwen3-compatible OCI/SIF checkpoint (2026-08-14)

```text
OCI:
  ghcr.io/matianxing1992/ndnsf-di-spec170@sha256:f1c8288e26d3dd7700b9519e51296fc3ed17027c60780fac4b361f470c628a26
source:
  a46abe9110ff816145a42b42e9b365d847e41135
SIF:
  /project/tma1/ndnsf-di/releases/spec170-runtime-a46abe9110ff816145a42b42e9b365d847e41135/runtime.sif
SIF SHA-256:
  sha256:5e556c17492957d07cde1debad5f7d93d794d43f87b3186914065d53e812fb4c
materialization record digest:
  sha256:9ab379c74ef7952c9eee825a75b69b2c2e832f1b8539dffe1e9ea556f9ee34aa
```

The release artifact checksums and the canonical materialization record verify.
Job `189293` reached a valid promoted SIF but ended `127` after the record had
been written because an optional helper was not staged; that failure and its
promotion-failure record are retained in the dedicated evidence file. The
artifact is accepted as an exact-SIF input for the next probe, not as a clean
Slurm orchestration PASS.

## Immutable OCI checkpoint (2026-08-13)

The accepted OCI candidate and its evidence are recorded in
`gpu-release-20260813.md`:

```text
ghcr.io/matianxing1992/ndnsf-di-spec170@sha256:94ce0cc847d453df90fc1aab74fade597f45e3199274ad782094fb45dd9bf916
```

The exact SIF materialization and static probe are recorded below. No mutable
tag or old Spec110/Spec168 image was substituted.

## Local exact-SIF availability check (2026-08-05)

The repository was checked before any exact-SIF claim:

```text
find results -type f \( -name '*.sif' -o -name '*.img' \)
  -> no Spec170 SIF artifact
command -v apptainer
  -> unavailable on this host
```

The existing local Docker image is a Spec168 image and is not substituted for
the T024 candidate. No SIF was rebuilt or retagged, and no TigerCluster job was
submitted.

## T024 implementation checkpoint

The deployment-side CPU/GPU boundary now has the following source contracts:

- `run-container.sh --gpu-count 0` omits Apptainer `--nv`; positive counts add
  `--nv` and always verify the supplied SIF SHA-256 before execution.
- Slurm rendering omits `#SBATCH --gres` for zero-GPU profiles and passes the
  explicit GPU count to the container launcher.
- `spec170_allocation_topology.py` rejects gate/profile mismatches, hidden
  defaults, invalid digests, and cross-Provider-to-local-GPU relabeling.
- Immutable D0/D1/D2a/D2b/D2h job entrypoints and exact-SIF mutation tests are
  present, but they have not been run against a materialized T024 SIF.

The source-only checks passed:

```text
bash -n specs/170-reusable-layer-artifacts/jobs/gate-d*.sbatch \
       packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-container.sh
pytest -q tests/container/unit/test_spec170_allocation_topology.py \
         tests/container/unit/test_spec170_exact_sif_gate.py \
         tests/container/unit/test_slurm_render.py \
         tests/container/unit/test_slurm_submit.py \
         tests/container/unit/test_slurm_node_scripts.py
13 passed
```

The source-only checkpoint hashes are:

```text
c08b76fc5a31e90817d569dc5c77c82d967cd7b72d2b9a85796b35489a2e0629  packaging/ndnsf-di-container/lib/spec170_allocation_topology.py
144625f559bf6fc7246c65121cb30ac52307950bc220378c84b6f74fc3573499  packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-container.sh
b27a5de77cb4fee6ac12f43401829a04a2ee055c6ab4b7734f4be3861212b1a1  packaging/ndnsf-di-container/lib/adapters/slurm_apptainer.py
0decd888b55c81c49fdb860f6873b1d3cc1cdb497714db4840ab535c03854fcd  packaging/ndnsf-di-container/adapters/slurm-apptainer/templates/ndnsf-di.sbatch.in
4440b2a62b481676d1f4f18b4469ca632e741df96d2a174ac9ae5abc9ff72ab8  specs/170-reusable-layer-artifacts/jobs/gate-d0-cpu.sbatch
528ec67fb0bd89447af082e3ae2959dc277acb4431d31b92a470eadc197058e8  specs/170-reusable-layer-artifacts/jobs/gate-d1-single.sbatch
769cc40786ca5a8208c1fc4f51964577da43c73169ecb65b18301bcdbf8e9985  specs/170-reusable-layer-artifacts/jobs/gate-d2a-local-two-gpu.sbatch
c7263f2bdce939689ce654fc7e54857698cdc8139137c6c751dd7b9ec0a3bc88  specs/170-reusable-layer-artifacts/jobs/gate-d2b-cross-provider.sbatch
23493a9f6a0282266d49b7f68364de978bd2770ca167aa82906f3674fccaca30  specs/170-reusable-layer-artifacts/jobs/gate-d2h-hybrid.sbatch
96cb59f32b1c2a296246bc17bd0327f8a228be5555d032fb9d05aed244e2dfc6  tests/container/unit/test_spec170_allocation_topology.py
7b18323a045b17fe063c39199ce81807c339118ab50d9da8e379be0dd74d7bb2  tests/container/unit/test_spec170_exact_sif_gate.py
```

## Exact SIF materialization and no-GPU static probe (2026-08-14)

The accepted OCI release was materialized once on TigerCluster compute node
`itiger05` by Slurm job `189255` using the local-scratch build path. The job
completed with exit code 0, peak RSS `25588064K`, and no OOM. The local SIF and
the promoted project copy were byte-identical:

```text
OCI:
  ghcr.io/matianxing1992/ndnsf-di-spec170@sha256:94ce0cc847d453df90fc1aab74fade597f45e3199274ad782094fb45dd9bf916
SIF:
  /project/tma1/ndnsf-di/releases/spec170-runtime-e23d759bb61159c8b3093e599fe301599d8c043f/runtime.sif
SIF SHA-256:
  sha256:525c4b890c4012d3f36653d0209f7decec508635818e5b0829250ef06d012af1
materialization:
  schema ndnsf-sif-materialization-v2; verified=true
  recordDigest sha256:0f9ca6b32fd58861496ac4b9f326d20b76bed840ece07ac88cb1d4c760562612
```

The exact SIF was then executed without `--nv` in Slurm job `189260` on
`itiger05`, with Apptainer `1.5.3-1.el9`, `--cleanenv --containall --home /tmp`,
and `/usr/local/bin/ndnsf-di-probe-runtime --mode static`. The probe exited 0
and reported `status: PASS`, all required native binaries, all required Python
imports, `torch 2.6.0+cu124`, and `onnxruntime 1.20.0`. It intentionally reports
`modelWeightsIncluded: false`; this is a runtime-image probe, not a model
execution result.

Three bounded probe attempts before the final invocation are retained as
negative launch evidence in `tiger-sif-static-probe-20260814.md`; they exposed
only missing/overridden container HOME and working-directory setup, not a SIF
digest or runtime-content failure.

Gate C remains open until the exact candidate is checked for the full T027
native/Python V3 parity and bounded CUDA-preflight contract without rebuilding.

## Bounded CUDA preflight (2026-08-14)

The same immutable SIF passed the bounded allocated-GPU probe in Slurm job
`189262` on `itiger01` with one `--gres=gpu:1` allocation and Apptainer
`--nv`. The probe reported `status: PASS`, `cpuFallback: false`, one UUID
(`GPU-90597ffb-6498-9a24-ca98-18fbdc33c447`), NVIDIA H100 80GB HBM3,
driver `560.35.03`, Torch CUDA `12.4`, and ONNX Runtime providers
`CUDAExecutionProvider` plus `CPUExecutionProvider`; the ONNX profile observed
CUDA execution. Non-fatal ONNX Runtime thread-affinity warnings were emitted
by the constrained Slurm CPU mask, but the CUDA kernel/provider and UUID checks
passed. This is only the bounded runtime preflight, not a frozen D1 workload.
