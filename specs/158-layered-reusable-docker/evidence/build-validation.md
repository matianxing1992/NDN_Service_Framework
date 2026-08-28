# Build and Validation Evidence

**Date**: 2026-07-27  
**Authority**: Local development candidate; no local NVIDIA GPU  
**Verdict**: PASS for the Spec 158 local build and reuse contract

## Accepted foundation products

| Product | Tag | Immutable local image ID | Size |
|---|---|---|---:|
| ML devel | `ndnsf-di-ml:spec158-16a56a51524d-devel` | `sha256:790fee01f5a3dffeda9da1de0b7ddc76d67b50eae4a05df20ce18262bb1a5c30` | 8.07 GB |
| ML runtime | `ndnsf-di-ml:spec158-16a56a51524d-runtime` | `sha256:c1960e61605daab59c404ad8429e1f290bd9530defc4cfa6b56be0e49262b021` | 8.06 GB |
| NDN devel | `ndnsf-di-ndn:spec158-968219322a79-devel` | `sha256:633983fe2a38bdc860e78955272ec1d115eea3a70ad83372b6cee63cb8e6793f` | 13.3 GB |
| NDN runtime | `ndnsf-di-ndn:spec158-968219322a79-runtime` | `sha256:be01a3ee9dc9f2ebf74cbb64c16779687a0a68627857554275fccbd7325eeb07` | 8.42 GB |

The stable NDN layer contains ndn-cxx, NFD, OpenABE/RELIC, NAC-ABE, and
websocketpp. It excludes ndn-svs, NDNSD, NDNSF, application source, models,
and deployment credentials.

## Accepted App products and reuse proof

| Candidate | App image ID | Total driver time | Docker App build |
|---|---|---:|---:|
| `spec158-app-clean2` | `sha256:419b0c1cd157013f6cfd331b9e34851455e632b6ea43578fc938f29ff1f32e13` | 60.619 s | 19.214 s |
| `spec158-app-reuse-proof` | `sha256:8f8d2a0b219dc6ee0216c43a3ead9d65125850431f30cc26b7aa4b88c0f2f6e4` | 59.004 s | 18.183 s |

The second build changed only `APP_BUILD_ID`. Its machine-readable reuse proof
records the exact same four foundation image IDs, a distinct App image ID, and
`executedProducts: ["app-runtime"]`. All native and Python compilation stages
in the App builder were cached; no ML or stable NDN compilation ran.

Canonical evidence:

- `results/spec158-layered-reusable-docker/app-clean2-20260727T0035Z/build-manifest.json`
- `results/spec158-layered-reusable-docker/app-reuse-proof-20260727T0040Z/build-manifest.json`
- `results/spec158-layered-reusable-docker/app-reuse-proof-20260727T0040Z/reuse-proof.json`

## Runtime and content gates

- Content scan: PASS, `bannedPathCount: 0`.
- Static probe: PASS as UID/GID `65532:65532`.
- Read-only-root run: PASS with only explicitly supplied temporary writable
  mounts.
- Required binaries: `App_ServiceController`, `di-native-provider`, `nfd`,
  and `nfdc`.
- Python imports: `ndnsf`, `ndnsf_distributed_inference`, PyTorch
  `2.6.0+cu124`, ONNX Runtime `1.20.1`, Transformers `4.48.2`.
- All nine NDNSF-DI owner profiles report version `0.111.0`.
- No model weights, credentials, Git metadata, dependency sources, or
  compiler caches remain in the App runtime.
- The App image uses the ONNX Runtime CUDA backend contract; CPU fallback is
  not accepted.

## Tests

The focused Spec 158 contract suite passes all 10 tests:

```text
python3 -m unittest discover -s tests/container/layered -p 'test_*.py'
Ran 10 tests
OK
```

The repository-wide `tests/container/run.sh offline` currently reports 59
passes, 4 failures, and 7 errors across 70 tests. The 11 non-passing cases are
pre-existing Spec 110 release/fixture and Slurm textual-contract drift:

- legacy `release-valid.json` contains `candidateId`, which the current release
  schema rejects, and this propagates into operator/compose tests;
- the copied Spec 110 deployment-profile schema differs from the package copy;
- one Slurm assertion expects the literal adjacent option sequence
  `--cleanenv --nv`, while the implementation adds containment options between
  them.

None of these tests imports or exercises the new `oci/layered` implementation.
They are recorded as repository-wide legacy debt and are not hidden or
reclassified as Spec 158 success.

## Rollback and limitations

The accepted Spec 110 runtime remains locally available under both
`ndnsf-di:spec110-local-1a320e5d9e42f4f76e78aac62d9bb647e3b159f0` and its
GHCR tag, both pointing to local image ID `sha256:57f3804cf787...`. Spec 158
has now passed the local replacement gate, so this old local runtime is
technically removable, but deletion remains an explicit operator decision.

The current machine has no NVIDIA GPU. Live CUDA inference, registry
publication, iTiger scheduling, OCI-to-SIF materialization, and Slurm
acceptance remain unverified and are not implied by this PASS.
