# Spec180 Revision 112 Tiger-MVP Scope Audit

**Date**: 2026-09-03
**Verdict**: `BLOCK_PRE_TIGER`
**Next gate**: `S0-NATIVE`

## Observed state

- The first live Y-A attempt failed before a request because Python/NAC-ABE and
  NFD/current NDN-SVS resolved different `libndn-cxx.so.0.9.0` files.
- The corrected `build-system-j2` tree completes
  `./waf -o build-system-j2 build -j2` with exit 0. This proves the NDNSF tree
  builds under the resource limit; S0 remains open until the complete
  NFD/NDN-SVS/NDNSF/NAC-ABE/Python linkage closure is verified. The earlier
  `-j8` partial tree is non-qualifying.
- Current linkage confirms the remaining S0 owner: `/usr/local/bin/nfd` and
  `build-system-j2/libndn-service-framework.so` both resolve
  `/usr/local/lib/libndn-cxx.so.0.9.0`, but the active Python 3.8 extension
  still loads the old `build/libndn-service-framework.so.0.1.0` and
  `.local-boost171/lib/libndn-cxx.so.0.9.0`. The Python extension and its
  NAC-ABE/native dependencies must be rebuilt against the accepted
  `build-system-j2` closure before S0 can pass.
- No current role-correct signed YOLO package binds the catalogue trust input,
  Provider offer keys, canonical model, and all digests as one candidate.
- No current MiniNDN Y-A or Y-B run has reached a terminal Response.
- CodeGraph finds the production `run_minindn_case()` path and its process,
  routing, publication, Provider, User, terminal-marker, and cleanup calls, but
  no direct test that drives that function to a real terminal Response; current
  focused tests validate seams rather than the complete execution.
- The checked-in Tiger profile still requests three RTX 6000 GPUs, two YOLO
  requests, and a second Qwen job; it does not yet implement revision 112.
- Therefore no current SIF or Tiger result can qualify Spec180.

## Planning cause

The earlier completion boundary coupled YOLO implementation, Qwen3.6-27B,
three distinct GPUs, two requests per workload, a broad local matrix, two SIF
workloads, and final cross-model closure. This allowed extensive focused-test,
audit, and release work without closing one vertical execution path.

## Corrected target

One immutable candidate must complete one cold YOLO Y-B request on one Tiger
node with one RTX GPU and four independent Provider processes. The three model
roles use CUDA ONNX Runtime on the same GPU; `Merge` is an explicit CPU role.
The result must include ACK-derived placement, one-to-one role ownership,
repository input fetch, role dependency delivery, 1/1 numerical match, child
exit status, redaction, and cleanup evidence.

This is a functional multi-Provider deployment result. It is not evidence of
multi-GPU distribution or performance.

## Finite route

```text
S0 unified native closure
 -> S1 signed YOLO experiment candidate
 -> S2 MiniNDN Y-A -> Y-B -> critical Y-N
 -> S3 convergence PASS + YOLO-relevant local gate
 -> S4 local SIF + exact-SIF Y-B
 -> S5 remote readiness + one Tiger Y-B request + closure
```

Qwen, a second warm request, three-distinct-GPU placement, and performance
experiments are deferred. Any failure returns only to its owning gate; it does
not restart completed gates or open another audit loop.
