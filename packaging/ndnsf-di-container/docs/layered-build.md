# Reusable Layered NDNSF-DI Container Build

This is the local development build path for NDNSF-DI. It separates expensive,
slow-changing dependencies from frequently changed application code:

```text
Pinned CUDA/cuDNN runtime
  -> ML devel/runtime
  -> stable NDN devel/runtime
  -> App runtime
```

The five products are independent local OCI images:

| Product | Owns | Rebuild when |
|---|---|---|
| `ndnsf-di-ml:*‑devel` | Python, PyTorch, Transformers, ONNX Runtime GPU Python/C++ | CUDA, Python, ML, or ONNX Runtime lock changes |
| `ndnsf-di-ml:*‑runtime` | Runtime-only copy of the ML closure | Same as ML devel |
| `ndnsf-di-ndn:*‑devel` | ndn-cxx, NFD, OpenABE/RELIC, NAC-ABE, websocketpp headers/tools | Stable NDN/security lock changes |
| `ndnsf-di-ndn:*‑runtime` | Runtime-only stable NDN/security closure | Same as NDN devel |
| `ndnsf-di:spec158-*` | ndn-svs, NDNSD, NDNSF, bindings, NDNSF-DI, native adapters | Any application source or App lock changes |

Models, identities, secrets, results, Git metadata, and source trees are never
part of the final image. Models and deployment identities remain runtime
mounts. The final assembly also removes the `onnx` package's bundled backend
test models and ONNX Runtime example datasets; they are not runtime
dependencies.

## Routine App rebuild

First confirm that the four foundation images named by the current locks exist:

```bash
docker image ls \
  --format '{{.Repository}}:{{.Tag}} {{.ID}} {{.Size}}' |
  grep 'ndnsf-di-\(ml\|ndn\)'
```

Then rebuild only the mutable layer:

```bash
build_id="app-$(date -u +%Y%m%dT%H%M%SZ)"
packaging/ndnsf-di-container/oci/layered/scripts/build-layered-local.sh \
  --target app \
  --jobs 2 \
  --app-build-id "$build_id" \
  --output "results/spec158-layered-reusable-docker/$build_id"
```

The driver seals the current filtered working trees, including allowlisted
untracked source files while excluding build output, credentials, models, and
other forbidden content; it builds ndn-svs, then
NDNSD, then NDNSF/NDNSF-DI, and verifies that all four parent image IDs remain
unchanged. A dirty working tree is intentionally labeled a development
candidate and is not a formal release.

The App builder deliberately selects the core library, ServiceController, and
native provider Waf targets. Unrelated examples and smoke binaries are not part
of the runtime product and are not rebuilt during routine container assembly.
It installs the selected C++ artifacts directly, then builds the Python binding
once into the App prefix; Waf's unrelated editable Repo wrapper post-install is
not invoked.

The active NDN-SVS checkout is not edited. The sealed copy receives exactly one
digest-bound build-only compatibility patch that changes its Boost minimum from
1.74 to the host baseline 1.71. Any patch-byte drift or unexpected original
`wscript` content fails the build before compilation.

## Complete or selected foundation build

The first build needs network access and substantial disk space:

```bash
build_id="all-$(date -u +%Y%m%dT%H%M%SZ)"
packaging/ndnsf-di-container/oci/layered/scripts/build-layered-local.sh \
  --target all \
  --jobs 2 \
  --app-build-id "$build_id" \
  --output "results/spec158-layered-reusable-docker/$build_id"
```

Use `--target ml` only after changing the platform or ML locks. Use
`--target ndn` after changing stable NDN/security inputs; its exact ML parents
must already exist. Use `--target app` for normal development. On this
four-core host, `--jobs 2` is the safe default; the driver rejects values above
four.

Each target uses separate JSON locks and deterministic source seals under
`oci/layered/`. Tags are content-derived, write-once references. The driver
inspects the resolved image IDs before and after child builds and rejects tag
collisions or parent drift.

`APP_BUILD_ID` enters only after native and Python compilation. Changing the
identity for a proof or local candidate therefore reuses all compilation
layers when the App seal and locks are unchanged.

## Evidence and inspection

Every non-empty output directory is rejected, so a run cannot overwrite old
evidence. The main record is:

```bash
jq . \
  results/spec158-layered-reusable-docker/<build-id>/build-manifest.json
```

Check at least:

- `status` is `PASS`;
- `developmentCandidate` is understood;
- all five `images.*.imageId` values are present;
- `contentScan.status` and `staticProbe.status` are `PASS`;
- an App-only rebuild retains the same four foundation IDs;
- the second build log contains no ML or stable NDN compilation.

Compare two passing App manifests mechanically:

```bash
python3 \
  packaging/ndnsf-di-container/oci/layered/scripts/verify-app-reuse.py \
  --first results/spec158-layered-reusable-docker/<first>/build-manifest.json \
  --second results/spec158-layered-reusable-docker/<second>/build-manifest.json \
  --output results/spec158-layered-reusable-docker/<second>/reuse-proof.json
```

The local host has no GPU. Static imports, ELF closure, content scanning, and
the unprivileged health probe therefore do not constitute live CUDA, iTiger,
OCI publication, or SIF acceptance.

## Failure recovery

A failed child build does not invalidate an already built parent. Read
`build-manifest.json.reasonCode` and the named layer log, fix the owned lock,
seal, Dockerfile, or source input, and rerun into a new output directory. Do
not retag or overwrite a content-derived parent.

The App layer fails closed on:

- source-seal or lock drift;
- unsafe archive members;
- a changed or additional Boost compatibility patch;
- build-order violations;
- unresolved required libraries;
- missing Python/native runtime components;
- CPU fallback;
- model, credential, Git, cache, result, or source content in the final image.

## Cleanup

Do not run broad `docker system prune` as part of this workflow. First record
the exact candidate and references:

```bash
docker image inspect <tag> \
  --format '{{.Id}} {{json .RepoTags}} {{json .RepoDigests}}'
docker ps -a --filter ancestor=<tag>
```

Delete only an explicitly named, locally reproducible, unreferenced image after
the replacement has passed. Preserve the accepted Spec 110 runtime until that
decision is made explicitly. Never delete or mutate remote GHCR evidence from
this local cleanup path.

## Future registry and iTiger use

The current path deliberately builds and validates local images only. A later
release workflow may push immutable digests and use registry-backed BuildKit
cache. OCI-to-SIF materialization on iTiger must bind the exact accepted digest
and run its own GPU and Slurm acceptance; it must not infer success from the
local no-GPU checks.
