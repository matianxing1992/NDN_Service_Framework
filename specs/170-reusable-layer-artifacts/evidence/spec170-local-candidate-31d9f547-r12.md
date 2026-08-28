# Spec170 local candidate r12 (2026-08-17)

## Immutable identity

| Item | Value |
|---|---|
| Release | `spec170-runtime-31d9f547-post-selection-v10-20260817` |
| SIF | `/tmp/spec170-local-candidate-31d9f547-20260817-r12/runtime.sif` |
| SIF SHA-256 | `110fab9d72aecbd1448bbdf1c5a4f9bf2304321fa99f2c49660dc724828f25c3` |
| Source seal SHA-256 | `0ebb2cdce2f0cf963ead8a8d7532c1b500cbd78596799dcb93252fc8b5d57354` |
| Library lock SHA-256 | `22456a81592e10e7f8c7eb988b03d131f505a4924fe9a7cf222b8586dfd93264` |
| Build record SHA-256 | `5cc4e9aeab3c4dee673288786db99dfb3359611b8ae3ebb5a0a3befb934854d6` |
| Apptainer | local `1.3.4`; Tiger `1.3.4-1.el9` |
| Tiger action | Verify hash and execute only; no remote build or materialization |

The build record binds the r10 qualified base SIF
`bd949732fc89bb10ef48e92b51fb4557fb33819797882765bd629c73da43748c`,
the r12 definition, source seal, and refreshed v10 labels. The builder used
`apptainer build --force` and verified every declared `org.ndnsf.di.*` label
before accepting the output, closing the inherited-label defect that rejected
r11.

## Local closure and functional gates

* Provider, framework, and Python 3.10 extension hashes match their sealed
  inputs. Exactly one `_ndnsf*.so` is active in the SIF.
* `import ndnsf._ndnsf` succeeds inside the exact SIF.
* ONNX Runtime is `1.20.0`; TensorRT, CUDA, and CPU execution providers are
  present.
* The real Provider command
  `--check-only --wiring-check-only` loads four roles and four artifacts and
  terminates with `NDNSF_DI_NATIVE_PROVIDER_CHECK_OK`.
* The executable SIF library-closure validator reports `PASS`; its JSON SHA-256
  is `9bfd74481e5823cc68b661368e0c28080f398fb03d7d14c6569a202b96b85d7a`.
* The exact-SIF network gate reports `3 passed, 6 deselected in 22.93s`:
  Provider CLI contract, D0 four-Provider dependency chain, and D1
  single-Provider dependency chain. Both functional cases require Selection,
  all four planned dependency publications/fetches with nonempty payloads,
  and the final Response.

## Paired local and Tiger bundles

The local functional gate uses a CPU-only manifest and explicitly forbids CPU
fallback; its bundle manifest digest is
`1727328ca7f7aa458833a3c88f45865b090d8052d1455f1c893845949fed9ac4`.
The Tiger bundle uses the same plan, policy, trust schema, driver, and model
artifacts, but declares CUDA with no CPU fallback; its bundle manifest digest
is `9489261331bb22347dcb5f183688b28a16d03bb7a738fd5376125d52d1a44bf9`.
The bundle validator accepts `service-manifest.json` as the only intentional
difference. Both service-manifest sidecars were regenerated from their actual
bytes before the bundle manifests were sealed.

An earlier diagnostic mistakenly supplied the CUDA manifest to the no-GPU
local gate. D1 crashed while initializing its first CUDA runner and D0 reached
the outer timeout. Those results are configuration-negative evidence, not an
NDNSF dependency-path result, and were not used for acceptance.

## Boundary

These gates qualify r12 for hash-bound Tiger execution. They are not Tiger
D0/D1/D2 evidence. Each Slurm case still requires remote/staged SIF hashes,
allocation/device evidence, request/ACK/Selection/assignment/response traces,
and the case-specific terminal checks.
