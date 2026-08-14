# Tiger Qwen3-compatible SIF materialization (2026-08-14)

## Immutable OCI release

The repaired GPU runtime build completed successfully in GitHub Actions run
`31758620149` from source revision
`a46abe9110ff816145a42b42e9b365d847e41135`.

```text
OCI reference:
  ghcr.io/matianxing1992/ndnsf-di-spec170@sha256:f1c8288e26d3dd7700b9519e51296fc3ed17027c60780fac4b361f470c628a26
release ID:
  spec170-runtime-a46abe9110ff816145a42b42e9b365d847e41135
release manifest digest:
  sha256:9bfcdddb67c4c5d7fbf954cff09bc7f2f16c5823962c606f98f7a87fe72351e3
record digest:
  sha256:c96a4508ab7d07ce26d17780fb9794c5f052296f96ce4f8d3935d98c7cf1c2eb
SBOM digest:
  sha256:4d0c41528ea820f284511a97a8d5dc4f02502367f3a0227b72aef85696f67a4a
```

The release artifact SHA256SUMS, anonymous manifest, SBOM, signature bundle,
and runtime release record were downloaded and verified locally. The build
used the Qwen3-compatible lock (`transformers==4.51.0`,
`huggingface-hub==0.30.2`).

## Slurm materialization

The first submission, job `189292`, failed immediately with exit `127` because
the job wrapper tried to execute a project-staged script through a path that was
not available as an executable entrypoint on the compute node. It did not pull
or write a SIF.

The corrected job `189293` ran on `itiger09` with 8 CPUs, 64 GiB RAM, and a
30-minute limit. It fetched the exact digest, built the SIF in node-local
`/tmp`, and completed local-to-project promotion and both SHA-256 comparisons.
The terminal job status was `FAILED (127)` only after promotion because the
optional helper `materialize-sif.sh` was not staged beside the wrapper. The
wrapper had already written the record before invoking that helper; the final
artifact was independently verified below. This terminal failure remains
negative orchestration evidence and is not hidden as a clean Slurm PASS.

```text
SIF:
  /project/tma1/ndnsf-di/releases/spec170-runtime-a46abe9110ff816145a42b42e9b365d847e41135/runtime.sif
SIF size:
  4,396,134,400 bytes
SIF SHA-256:
  sha256:5e556c17492957d07cde1debad5f7d93d794d43f87b3186914065d53e812fb4c
Apptainer:
  1.5.3-1.el9
materialization record:
  /project/tma1/ndnsf-di/releases/spec170-runtime-a46abe9110ff816145a42b42e9b365d847e41135/materialization.json
record digest:
  sha256:9ab379c74ef7952c9eee825a75b69b2c2e832f1b8539dffe1e9ea556f9ee34aa
```

The project SIF hash equals the local build hash and the record's canonical
digest recomputes exactly. The promotion-failure record from job `189293` is
also retained beside the release record. This establishes exact SIF availability
for the candidate, but it does not yet establish static-runtime, CUDA, Qwen3
standalone, or MiniNDN Gate B acceptance.
