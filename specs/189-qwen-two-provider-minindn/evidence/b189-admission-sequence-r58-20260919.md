# B189-3 admission sequence repair and r58 resource boundary

**Date**: 2026-09-19
**Status**: `PARTIAL` / `RESOURCE_BOUNDARY`
**Candidate**: `qwen06b-553e2d82160a`, run `two-provider-global-r58`

## Static gate

The frozen review snapshot was
`.codex-tmp/spec189-admission-static-review-20260919-v4/`.
Its `changed.diff` SHA-256 is
`b380a53c511046c86f113595e000dc2310cbb505e9f6d1f264b83727c4c8db70` and its
seven source hashes are recorded in `sources.sha256`. The official read-only
review returned `STATIC_PASS`.

The repair attaches one runtime-only `shared_ptr<atomic<uint64_t>>` to each
authenticated Selection projection. The pointer is excluded from Selection
JSON and canonical digest computation. `GRANT_VERIFIED` reports admission as
epoch 1, sequence 1; every assembler/rebuild factory copy shares that counter
and therefore reports sequence 2, 3, ... instead of restarting at sequence 2.
Plaintext, legacy and no-factory paths do not emit this admission status.

## Local C++ validation

The affected closure was rebuilt from `build-spec189-b189-3-global-r3` with the
system-first toolchain:

```text
env PATH=/usr/bin:/bin:/usr/sbin:/sbin /usr/bin/python3 ../waf build \
  --targets=ndnsf-distributed-inference,di-native-provider,unit-tests,integration-tests -j3
```

The build completed successfully in 20m10.630s. The worker target was then
built successfully in 41.816s:

```text
../waf build --targets=di-native-assembly-worker -j3
```

The C++ lifecycle selector covering authenticated progress sequences 1→2→3
passed. From the repository root, with
`NDNSF_SPEC182_BIN_DIR=build-spec189-b189-3-global-r3`, the complete
`Spec175NativeAssembly/*` suite passed 9/9. The first invocation from the build
directory is retained as a test-environment failure because the worker and
relative `examples/trust-any.conf` were not discoverable; it is not counted as
a product result.

## Real run

The fresh run used the same model, stage manifest, topology, profile and oracle
as r56/r57, with only the rebuilt Provider and assembly-worker binary digests
updated. Both Providers reached their normal `READY` boundary. Before ACK or
Selection markers, the host guard stopped the run at:

```text
NATIVE_HOST_GUARD_STOP boundary=RESOURCE_BOUNDARY:diskFree cleanup=PASS
returncode=-2 remainingProcesses=[]
```

The guard profile required at least 4 GiB free disk. Samples reached
`diskFreeBytes=4,282,318,848` and later `4,275,970,048`, below the threshold;
the final sample was drained with no processes. Available memory stayed above
4.24 GiB and `swapUsedBytes=0`, so this run is a disk guard boundary, not a
stream, Repo, ORT, provider assembly or model verdict. No ACK, Selection,
`ASSEMBLY_ADMISSION`, `ASSEMBLY_STARTED`, runner, terminal output or
qualification was observed.

Raw run data is retained at
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r58/`.
The run remains `PARTIAL`; the next retry requires a newly recorded disk-safe
host state and must keep a new run ID. It must not be relabelled as a protocol
PASS or used to claim Qwen two-provider completion.
