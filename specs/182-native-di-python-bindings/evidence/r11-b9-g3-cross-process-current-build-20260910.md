# R11-B9-G3 Current-Build Cross-Process Revalidation (2026-09-10)

## Scope and result

`CLOSED_FOR_VALIDATION` for the local independent C++ process boundary. The run uses the
existing Spec182 build configured with the explicit NAC-ABE installation and NDN-SVS
source/build pair recorded in `build/config.log`. The fixture is intentionally tiny, but
Provider execution uses real ONNX Runtime CPU (`realCompute=true`, `cpuFallbackUsed=false`).
Python owns process lifecycle and fixture files; requester, Core, Authority, Provider,
stream decoding, continuation state, replacement and native oracles remain in C++.

This evidence does not close maintained callers, legacy zero-use, no-Python dependency
qualification, exact-SIF, MiniNDN/Slurm/GPU or T015--T017.

## Loader boundary and correction

The first invocation returned `rc=1` during `_ndnsf` import because an inherited
`LD_LIBRARY_PATH` selected `/usr/local/lib/libnac-abe.so` before the matching
`/home/tianxing/NDN/nac-abe-integration-182/install/lib/libnac-abe.so`; the former lacks
`ndn::nacabe::Consumer::clearCache(...)`. No native process or protocol result was counted.
The raw failure is retained at
`.codex-tmp/spec182-r11-b9-cross-process-current-20260910-r0/` and indexed in
[`docs/failure-log.md`](../../../docs/failure-log.md).

The retry placed the matching NAC-ABE and NDN-SVS directories first, followed by the
ndn-cxx/ndnsd directory. `ldd` then resolved `libnac-abe.so` to the explicit NAC-ABE
installation and `libndn-svs.so.0.1.0` to `/home/tianxing/NDN/ndn-svs/build`.

## Process results

| Scenario | Native result | Retained raw run |
| --- | --- | --- |
| `--conversation` | first requester `rc=0`, stream oracle tokens `4,5,6,7,8,9,10,2`, checkpoint written; second `APPEND_DELTA` requester `rc=0`; wrong parent `rc=1` with `DI_NATIVE_CONVERSATION_PARENT_MISMATCH` | `/tmp/spec182-r11-b9-cross-process-current-20260910-r1` |
| `--conversation --recovery` | first requester `rc=0`; Provider SIGKILL/restart; append `rc=1` with `NATIVE_STREAM_FAILED`; restarted Provider reports `PROVIDER_CONVERSATION_STATE_MISSING`; wrong parent rejected | `/tmp/spec182-r11-b9-cross-process-current-20260910-recovery` |
| `--replacement` | first Provider stopped after authenticated ACK; alternate Provider completes `attempt-2`, emits grant verification and ORT execution evidence; requester `rc=0` and stream oracle passes | `/tmp/spec182-r11-b9-cross-process-current-20260910-replacement` |
| `--replacement --replacement-no-backup` | requester `rc=1` with `NATIVE_REQUEST_STAGE_FAILED` / `DI_NATIVE_NO_ADMITTED_PROVIDER` | `/tmp/spec182-r11-b9-cross-process-current-20260910-replacement-no-backup` |

The Provider logs contain `NDNSF_DI_GRANT_VERIFICATION` before assembly and
`NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED` with `runnerKind=onnxruntime-cpu` and
`executionCompleted=true`. The recovery restart contains no execution or stream marker.

## Review and verification

- Current native process driver was inspected for process ownership, role-specific PIB/TPM,
  cleanup, continuation parent binding, recovery and replacement assertions.
- Marker assertion over all four retained run roots passed.
- Matching dependency closure check passed with `ldd`.
- The prior build's `config.log` records `/usr/bin/g++`, explicit NAC-ABE/SVS prefixes and
  the Spec182 ONNX prefix; no source or ABI files changed in this batch.

## Next gate

Use the same explicit dependency closure when migrating maintained callers. The next native
closure work remains caller-group evidence followed by no-Python/T014 checks; do not infer
multi-machine qualification from this local process result.
