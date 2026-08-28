# Layered Build Contract

## Command surface

```text
build-layered-local.sh
  --target ml|ndn|app|all
  --output <directory>
  [--jobs <positive integer>]
  [--app-build-id <identifier>]
  [--dry-run]
```

Default jobs: `2`. The script rejects an App build when accepted parent
manifests are missing or their recorded image IDs no longer exist.

## Products

```text
ndnsf-di-ml:spec158-<lock12>-devel
ndnsf-di-ml:spec158-<lock12>-runtime
ndnsf-di-ndn:spec158-<lock12>-devel
ndnsf-di-ndn:spec158-<lock12>-runtime
ndnsf-di:spec158-<app-id>
```

Tags are lock-derived build references and may not be overwritten. Parent
linkage and acceptance use the inspected image IDs recorded in the manifests.
Before and after a child build, the driver rejects any tag whose image ID no
longer matches its accepted parent manifest.

## Prefix contract

- ML: `/opt/venv`, `/opt/onnxruntime`, `/usr/local` Python runtime
- Stable NDN: `/opt/ndn-base` (ndn-cxx, NFD, OpenABE/RELIC, NAC-ABE)
- Mutable App: `/opt/ndnsf-app` (ndn-svs, then NDNSD, then NDNSF/DI)

The App build may read ML/NDN headers from its devel parent but may install only
to `/opt/ndnsf-app`. The runtime combines only runtime products and the App
prefix.

## Fail-closed gates

- digest-pinned upstream images;
- exact layer-lock hash;
- verified source archives with safe members;
- no layer ownership violation;
- no unresolved required DSO except the explicitly allowed host NVIDIA driver;
- exact Python package/version contract;
- no TensorRT or CPU fallback;
- no model/credential/Git/result/cache content;
- unprivileged static runtime probe.

NDN-SVS receives one build-only Boost 1.71 compatibility patch inside the
sealed App builder. Its path and SHA-256 are owned by the App lock. The build
requires exactly one occurrence of each expected Boost 1.74 source line,
applies with `--fuzz=0`, and verifies the exact 1.71 replacements. It never
modifies the active NDN-SVS checkout.

## Incremental reuse proof

The proof consists of two App builds with different App build IDs. It passes
only when:

1. all four foundation image IDs are identical before and after;
2. the second build log contains no execution of ML installation or stable NDN
   compilation;
3. both final images pass the same probe;
4. the manifest records distinct App identities and common parent IDs.
