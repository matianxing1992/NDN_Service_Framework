# Spec180 Revision 110: Delay Diagnosis and Ordered Gates

**Date**: 2026-09-03  
**Feature**: `180-ack-driven-cross-model-qualification`  
**Status**: `BLOCKED_AT_G0_NATIVE_CLOSURE`

The replacement system-prefix build is still an in-progress implementation
action. Its existence or partial object count is not a closure PASS; the gate
opens only after the final framework library, Python extension, NFD/NDN-SVS/
NAC-ABE linkage, import, and native startup smoke all agree.

The attempted `build-system-j8` tree is explicitly non-qualifying: it violated
the repository's `-j2` resource boundary, was interrupted before producing the
final framework library, and must not be reused as G0 evidence. The next build
must use one tree at `-j2` and record the complete command.

## Diagnosis

Spec180 has not yet produced a qualified NDNSF-DI experiment because the first
real request has not crossed the pre-network and execution gates. The delay is
not evidence that the ACK-driven protocol is incorrect.

1. The first Y-A attempt reached Controller/Repository startup but ended with
   native-client `socket read error (End of file)` before any request.
2. The Python extension/NAC-ABE resolved
   `.local-boost171/lib/libndn-cxx.so.0.9.0`, while NFD and the current
   NDN-SVS resolved `/usr/local/lib/libndn-cxx.so.0.9.0`. The files have
   different SHA-256 digests and ELF Build IDs. This is one split ABI/toolchain
   closure, not a protocol result.
3. The candidate-input path is independently incomplete: temporary packages
   are stale-role or unsigned, and no owner-supplied manifest binds the
   registered signing key, Provider offer-key map, and canonical Y-A model.
4. The former task graph allowed focused audits, negative seams, and release
   preparation to advance before one atomic Y-A request. This created
   `implemented`/`wired` evidence without `executed` terminal-response
   evidence.

## Mandatory recovery order

```text
G0-NATIVE       one identical native library closure and startup smoke
G0-CANDIDATE    trusted role-correct package, signature, keys, policy, model
G1-Y-A          one real Controller -> Repository -> Provider -> User response
G2              reuse the same driver for Y-B and fixed Y-N controls
T013            release tooling, QWEN-F production input/entrypoint, result writer
T014            one design-code convergence PASS
T015--T020      one frozen local/SIF/Tiger route
```

The first open gate is the only active gate. A missing or mismatched input is
recorded once as `WAITING_EXTERNAL_INPUT` (exit 78); a failure after MiniNDN
starts is `UNQUALIFIED`. Neither status is a successful experiment. Do not
use `LD_LIBRARY_PATH` to mask the split closure, edit a manifest in place,
relabel a temporary package, rebuild SIF, submit Tiger, or expand a matrix
before the owning gate passes.

## Evidence boundary

Focused tests and fake publication transports remain useful implementation
evidence. They do not prove live NDN publication, ACK closure, Selection,
Provider execution, terminal Response, or cleanup. Spec175 remains a frozen
local baseline and is not reopened to repair any item above.
