# R11-B9-G1 Portable ELF Closure Gate

Date: 2026-09-10

## Finding

The existing runtime closure checker used `ldd` to decide whether an ELF
dependency could be resolved on the current host. That was insufficient for a
portable image: an ELF could still carry an absolute host or build path in
`DT_RPATH`/`DT_RUNPATH`, and a local machine could make that path appear valid.
Such a candidate could fail after transfer to another node or into an SIF.

## Repair

`verify-runtime-closure.py` now accepts repeatable `--reject-prefix` options.
When enabled, it reads ELF dynamic path tags with `readelf` and checks both
those values and absolute paths resolved by `ldd`. It fails closed with
`RUNTIME_HOST_BOUND_PATH` for configured host/build prefixes. The GPU and
layered assembler/devel stages invoke this gate before deriving runtime Debian
packages, using `/home/`, `/workspace/`, `/build/`, `/src/`, and `/tmp/`.
The final runtime stages retain their normal unresolved-library scan; the
strict inspection runs in build stages where `binutils` is available and does
not add a build-only inspection tool to the runtime image.

## Verification

```text
python3 tests/container/itiger-qwen-live/unit/test_runtime_package_closure.py
  5 tests, OK

python3 packaging/ndnsf-di-container/oci/scripts/preflight-gpu-build.py \
  --workspace . --output /dev/null
  PASS (nativeTargetCount=35, repoTargetCount=4)

python3 -m py_compile \
  packaging/ndnsf-di-container/oci/scripts/verify-runtime-closure.py \
  packaging/ndnsf-di-container/oci/scripts/preflight-gpu-build.py \
  tests/container/itiger-qwen-live/unit/test_runtime_package_closure.py
  PASS
```

The unit test builds a real ELF with `/home/spec110-host/lib` in its runpath
and verifies that the configured `/home/` prefix is rejected. No OCI build,
SIF transfer, Slurm run, GPU driver injection, or exact-SIF/no-Python
qualification was run. This card therefore closes only the static host-path
gate; T014/T015/T016/T017 and the parent R11-B9 remain open.
