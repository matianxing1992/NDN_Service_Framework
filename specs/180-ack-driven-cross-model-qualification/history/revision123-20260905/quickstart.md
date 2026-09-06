# Quickstart: Spec180 YOLO Tiger Functional Slice

**Spec180 revision**: 112
**Current state (2026-09-03)**: `S0` passed on the host with the Python 3.8
extension linked to `build-system-j2` and every consumer resolving the same
hashed `/usr/local/lib/libndn-cxx.so.0.9.0`. Execution is now stopped at `S1`
until the signed current-role YOLO candidate closes. No SIF build, upload,
staging, or Slurm submission is allowed before `S1`--`S3` pass.

This is the only accepted route. Do not replace checked-in commands, profiles,
or manifests with ad hoc variants.

## Target

One immutable candidate completes one cold YOLO Y-B request on one Tiger node:

```text
User
  -> ACK collection and shared-backbone plan
  -> BackboneNeck Provider ------+
                                 +-> DetectShard0 Provider --+
                                 +-> DetectShard1 Provider --+-> Merge Provider
                                                               -> Response
```

The four roles have four independent Provider processes. The three model roles
use CUDA ONNX Runtime on the same allocated RTX GPU. `Merge` runs its declared
CPU postprocessing. Passing this route proves functional deployment, not
multi-GPU scaling or performance.

## S0 — Native closure

1. Use one active build tree and at most two compiler jobs. The canonical Waf
   command shape is `./waf -o build-system-j2 build -j2`; `-j8`, `nproc`,
   overlapping builds, and the invalid extra command
   `build ndn-service-framework -j2` are forbidden.
2. Build NFD, NDN-SVS, NDNSF, NAC-ABE, and the Python extension from one
   explicit toolchain/dependency prefix.
3. Record the resolved `libndn-cxx` path, SHA-256, SONAME, and Build ID for all
   five consumers.
4. Require the Python import and a small native client startup to pass.

A `.local-boost171` versus `/usr/local` split is
`WAITING_EXTERNAL_INPUT`; do not mask it with `LD_LIBRARY_PATH`.

## S1 — Immutable YOLO candidate

Seal one candidate containing:

- the current canonical YOLO ONNX graph and external initializers;
- current role names `FullModel`, `BackboneNeck`, `DetectShard0`,
  `DetectShard1`, and `Merge`;
- the atomic and shared-backbone candidate descriptions;
- a full-model numerical oracle;
- Provider offer public keys and an experiment catalogue trust root;
- the encrypted repository-backed input reference;
- all file, graph, initializer, policy, and key digests.

The experiment owner may create a fresh functional-test trust set while sealing
the candidate. Private keys remain outside Git with restricted permissions;
their public identities and digests are immutable candidate inputs. This does
not claim production PKI deployment.

Reject stale `DetectHead0/1` packages, unsigned catalogues, missing Provider
keys, in-place manifest edits, or a model path outside the sealed input set.

## S2 — One local vertical driver

Use `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` and the registered case
contract. Run in this order:

1. `Y-A`: one `FullModel` Provider and one terminal Response;
2. `Y-B`: four Provider processes and the shared-backbone terminal Response;
3. critical `Y-N` controls only: infeasible capabilities, invalid catalogue or
   assignment signature, wrong object digest, duplicate/wrong role owner, and
   cleanup failure.

Every positive case must use real NFD, NDN-SVS, security, ACK/Selection,
repository input fetch, Provider execution, NDN role dependencies, numerical
comparison, child supervision, and cleanup. A focused seam test is not an
executed case.

## S3 — Freeze the local subject

1. Run the code-aware T014 convergence audit against the production YOLO path.
2. Repair every controlling finding with a focused regression.
3. Require audit `PASS`.
4. Run the registered YOLO-relevant unit/integration selectors and Y-A/Y-B/Y-N
   inventory exactly once from the accepted source identity.
5. Freeze source, dependencies, candidate, runner, profile, and result schema.

Any behavior-affecting change after this point returns to its owning earlier
gate.

## S4 — Build one SIF

Build one SIF locally from the frozen inputs. Never copy a host-built
`_ndnsf.so`, host venv, host `site-packages`, or host runtime library into it.
Inside the final image verify:

- Python/SOABI/extension identity and import;
- complete `ldd`/RPATH closure;
- NFD, NDN-SVS, NDNSF, NAC-ABE, and `libndn-cxx` identities;
- CUDA and ONNX Runtime execution-provider inventory;
- absence of PyTorch, Transformers, and Ultralytics from the deployed runtime;
- the real runner/configuration probe;
- one exact-SIF Y-B replay without source/package overlay.

Seal the SIF SHA-256. A failed image is not uploaded or patched in place.

## S5 — Readiness and one Tiger request

The fixed profile is:

```text
nodes: 1
GPUs: one GPU (1 RTX)
Provider processes: 4
model roles: BackboneNeck, DetectShard0, DetectShard1 -> CUDA device 0
CPU role: Merge
requests: 1 cold Y-B request
```

Before any scheduler call, the checked-in submission closure verifies the SIF
hash, YOLO artifact/signature/digests, encrypted input reference, Apptainer
1.5.3, `/bundle` working directory, NFD routes, identities, environment
allowlist, CUDA visibility, evidence root, and cleanup contract. A rejection
must cause zero remote mutation and zero scheduler calls.

Stage only accepted bytes. Submit once through the checked-in entrypoint. Pass
requires:

- ACK-derived shared-backbone selection;
- four unique Provider processes and one-to-one role ownership;
- selected-ingress-only input fetch;
- correct role assembly and NDN dependency delivery;
- CUDA ONNX Runtime for all three model roles and zero CPU model fallback;
- 1/1 numerical match against the full-model oracle;
- complete lifecycle, device, child-exit, redaction, and cleanup evidence.

One byte-identical resubmission is allowed only for a recorded infrastructure
failure before workload entry.

## Closure language

Emit exactly one `FUNCTIONAL_PASS` or `UNQUALIFIED` verdict with the first
failing layer. Do not claim Qwen qualification, multi-GPU distribution,
warm-cache behavior, throughput, latency, scaling, or performance from this
result.
