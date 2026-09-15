# Spec186 successful-template comparison — 2026-09-15

## Purpose and authority

This is a pre-build comparison receipt. It records why the next candidate must
follow the retained Spec183 successful GPU recipe; it does not reuse its job IDs
or promote its runtime result to Spec186. The reference is
[`Experiments/TigerCluster/docs/successful-tiger-gpu-template.md`](../../../Experiments/TigerCluster/docs/successful-tiger-gpu-template.md).

## Frozen reference tuple

| Plane | Reference | Spec186 candidate rule |
| --- | --- | --- |
| Base SIF | `base-runtime-controller-version-j4-v23.sif`; SHA-256 `44b44d564c64387716a17588ea387ee2a255948ee744855291c8e7a0676907b0`; 3,586,351,104 bytes | Use the exact project-storage file; reject the r38 intermediate (`328f2ffd…`) or a filename-only match. |
| Builder | Clang 10 / `clang++-10`; `-O0 -g0 -B/usr/bin`; max Waf parallelism 1 for native target | Keep compiler and flags in the rendered definition unless a new candidate explicitly records a reason and requalification boundary. |
| Container | Apptainer 1.5.3 on allocated compute | Login Apptainer 1.3.4 remains metadata-only. |
| Runtime shape | complete two-stage `localimage`; builder compiles dependencies/Core/extensions; final copies only staged outputs | Render from `development-runtime.def.in`; no manual edits to a failed rendered file. |
| Acceptance shape | historical jobs 210365 (single-node) and 210366 (two-node); observed CUDA model roles, CPU Merge, numerical oracle, exits and cleanup | New candidate must collect its own MiniNDN/Tiger evidence; historical receipts are comparison only. |

## Candidate comparison and rejected deviations

The current source seal is `sha256:610161e75d5ce505019856ad86b64331c9b1b82dc7d4415033a05570c626ee9e`
(`0261ec6e` source revision). It is a legitimate new source plane. The r49/r50
definitions then introduced unapproved build-plane deviations: r49/r50 used the
r38 intermediate base, r49 used GCC, and r49 passed `--toolchain-root=/usr`.
The r49 closure failure (`ld`/`stdlib.h` missing) and r50 cancellation are
retained in [`docs/failure-log.md`](../../../docs/failure-log.md). They are not
candidate evidence.

The next definition must be produced with the maintained handoff renderer after
updating a candidate-only lock to the exact reference base SHA. Its diff must
show only the new source seal, release/run identity, current app/model/profile
inputs and other explicitly declared Spec186 planes. The base path, compiler,
toolchain root, staged transfer list and multi-stage structure must remain the
template values.

## r51 render receipt

The candidate-only handoff was updated to release `spec186-runtime-r51` and the
exact reference base. On Tiger, the maintained renderer produced
`/home/tma1/.cache/spec186-r51/spec186-r51-runtime.def` with SHA-256
`57e8b1f517f9c09ba4219c8ab5e283edc7229b746ce0d038bcca73a40d7faec6`. Its diff
against the template contains only the expected bundle/base/seal/release path
substitutions; the compiler, `--toolchain-root=/usr`, `CXXFLAGS`, Waf source/build
pair and stage transfer list are unchanged. The exact project base was checked
on compute as 3,586,351,104 bytes with SHA-256
`44b44d564c64387716a17588ea387ee2a255948ee744855291c8e7a0676907b0`.

The follow-up capability probe (`212386`/`212387`, Apptainer 1.5.3) showed that
this reference base contains neither Clang 10 nor the `/opt/onnx` and
`/opt/rust-prefix` build inputs. The existing r38 intermediate contains the
ONNX/Rust inputs and GCC but still no Clang. This capability mismatch is now a
required pre-build check; it explains why exact r51 could not proceed past APT
under the root-mapped account and why a derived toolchain base, if used, must
be a new candidate plane.

The remote pre-build command was:

```bash
python3 Experiments/TigerCluster/adapters/slurm-apptainer/scripts/preflight-development-sif.py \
  --definition /home/tma1/.cache/spec186-r51/spec186-r51-runtime.def \
  --apptainer /home/tma1/.local/bin/apptainer-1.5.3 \
  --base-sif /project/tma1/ndnsf-di/candidates/spec183-v52-20260911/planes/runtime/base-runtime-controller-version-j4-v23.sif
```

It returned `SPEC186_PREFLIGHT_PASS`; the subsequent exact-template build job
`212385` failed at root-mapped APT before compilation. No native build or
runtime qualification has been claimed from this receipt.

## Checks required before an expensive build

```text
template tuple -> candidate lock/base SHA -> handoff render
-> definition diff -> bash/Python/boundary checks -> SIF preflight
-> build-local-sif.sh -> exact import/help/ELF closure
```

Any failed check receives a new run/candidate identity and a failure-log entry
before another submission. A build/import receipt cannot close T007–T012.
