# Spec170 Tiger native-tracer multi-Provider wiring evidence (2026-08-14)

## Candidate binding

- OCI: `ghcr.io/matianxing1992/ndnsf-di-spec170@sha256:f1c8288e26d3dd7700b9519e51296fc3ed17027c60780fac4b361f470c628a26`
- source revision: `a46abe9110ff816145a42b42e9b365d847e41135`
- SIF: `/project/tma1/ndnsf-di/releases/spec170-runtime-a46abe9110ff816145a42b42e9b365d847e41135/runtime.sif`
- SIF SHA-256: `5e556c17492957d07cde1debad5f7d93d794d43f87b3186914065d53e812fb4c`
- plan: `sha256:a31c443ba04da7a66b4457fe859d5b63d2d773e325b991c0036891c8a812d216`
- service manifest: `sha256:1a9f689551796d990f4f2648e97d3120d9be4ef1dc576d8a41390831f38edbfe`
- bundle: `/project/tma1/ndnsf-di/evidence/spec170/native-tracer-check-20260814/`

## Test

The exact candidate SIF and the same four-role NativeTracer plan were used for
four concurrent `di-native-provider --check-only --wiring-check-only`
processes: `/Backbone`, `/Head/Shard/0`, `/Head/Shard/1`, and `/Merge`.
Each process was given a distinct job-local home directory, bound as its
container home, and a shared artifact/cache directory. No GPU or real model
compute was claimed; this is a provider registration and backend-wiring check.

## Evidence chronology

| Slurm job | Result | Finding |
|---|---|---|
| 189310 | failed | The four processes shared `/tmp/.ndn`; three failed with `PIB database cannot be initialized: database is locked`. This is an environment-isolation failure, not a protocol result. |
| 189311 | invalid wrapper result | `APPTAINERENV_HOME` overrode the attempted `--env HOME`; all four processes failed to create `/home/tma1/.ndn`. The wrapper also returned the status of `echo` rather than the child process and printed a false PASS. |
| 189312 | passed | A single-provider check using `env -u APPTAINERENV_HOME -u SINGULARITYENV_HOME` and `--home <job-local>:/home/tma1` completed with `NDNSF_DI_NATIVE_PROVIDER_CHECK_OK`. |
| 189313 | invalid wrapper result | With isolated homes, all four providers emitted `NDNSF_DI_NATIVE_PROVIDER_CHECK_OK`, but a multi-file `grep -c` aggregation treated four output lines as one integer and falsely returned failure. |
| 189314 | passed | Corrected per-provider exit-code and marker checks returned `NDNSF_DI_NATIVE_TRACER_MULTIPROVIDER_CHECK_PASS providers=4 roles=4 isolated_state=1`. |

## Interpretation

The candidate runtime can register all four NativeTracer providers and load
the ONNX Runtime backend in a concurrent check-only run when PIB state is
isolated. Jobs 189310, 189311, and 189313 are retained as negative wrapper or
environment evidence. This does **not** establish end-to-end MiniNDN,
authorization, request/response, GPU inference, or distributed aggregation;
those remain Gate B/D work.

