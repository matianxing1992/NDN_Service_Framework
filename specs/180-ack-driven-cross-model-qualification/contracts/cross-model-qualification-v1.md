# YOLO Tiger Functional Qualification Contract v1

The filename is retained for compatibility with existing validators. Spec180
revision 112 narrows the active contract to one YOLO functional slice. Qwen
and cross-model qualification are deferred.

## Claim boundary

The feature may claim only that one immutable NDNSF-DI runtime candidate can
execute one registered ACK-driven four-Provider YOLO request correctly on a
real Tiger GPU node. Timing values are diagnostic. There is no Qwen,
multi-GPU, warm-cache, throughput, latency, scaling, efficiency, or superiority
claim.

## Local formal cases

| ID | Required behavior |
|---|---|
| Y-A | One `FullModel` Provider; encrypted repository input reference; request before ACK closure; atomic candidate; terminal oracle-equivalent result |
| Y-B | Four independent Providers; post-ACK shared-backbone selection; one-to-one role ownership; NDN dependencies; terminal oracle-equivalent result |
| Y-N | Critical infeasible-capability, signature/provenance, object-digest, wrong/duplicate-role-owner, and cleanup controls reach their declared fail-closed outcomes |

All cases use real NFD, NDN-SVS, current security, production User/Provider
entrypoints, child supervision, and cleanup. CPU ONNX Runtime is intentional
and recorded locally; it is not Tiger GPU evidence. Focused in-memory tests are
not MiniNDN execution evidence.

## Exact-SIF case

The locally built SIF runs Y-B without source/package overlay. The record binds
the candidate and SIF hashes; in-image Python/SOABI/extension, NFD/NDN-SVS/
NDNSF/NAC-ABE library closure, ONNX Runtime provider inventory, real runner,
result oracle, every child exit, and cleanup.

## Tiger job YOLO-F

- one node, one Slurm task, one GPU (one RTX GPU allocation), and 32 GiB host
  memory;
- four independent Provider processes and one-to-one ownership of
  `BackboneNeck`, `DetectShard0`, `DetectShard1`, and `Merge`;
- the three model roles use CUDA ONNX Runtime with
  `CUDA_VISIBLE_DEVICES=0` and record the same physical GPU UUID;
- `Merge` has no visible CUDA device and performs only the declared CPU
  dependency-consumer/postprocessing work;
- 5000 ms initial SVS settle, 1500 ms ACK timeout, 60000 ms request timeout;
- one cold request using the registered encrypted repository input reference;
- 1/1 numerical match within `atol=1e-3` and `rtol=1e-4`;
- no CPU model fallback, plaintext leakage, uncollected child, or owned
  survivor.

## Verdicts

`FUNCTIONAL_PASS` requires the local, exact-SIF, and Tiger records to bind the
same candidate and for all protocol, numerical, runtime, device, child-exit,
redaction, and cleanup oracles to agree. Otherwise the verdict is
`UNQUALIFIED` with the first failing layer and preserved evidence.

Every oracle is a schema-checked, digest-verified evidence reference. A bare
`PASS` label is insufficient. One byte-identical resubmission is allowed only
for a recorded infrastructure failure before workload entry.

## Terminal record encoding (revision-123 T014 repair, 2026-09-04)

The maintained `scripts/validate_spec180_results.py` accepts only
`gate=yolo-functional` and integer request/terminal counts of one. The exact
child IDs are `nfd`, `controller`, `repo`, `user`, and
`provider-{BackboneNeck,DetectShard0,DetectShard1,Merge}`; every child needs an
integer zero exit status and `timedOut=false`. A subset is not an inventory.

Each referenced `spec180-oracle-v1` JSON file contains exactly the reference's
`schema`, `version`, `status`, `candidateId`, and `fields` (not its `path` or
`sha256`). The file digest and those contents must both match. An unrelated
file with a correct digest cannot attest the submitted PASS fields.

`runtimeOracle.fields` contains `backend=cuda-onnxruntime`, `cpuFallback=false`,
`deviceIds=[<one physical GPU UUID>]`, and four `providers` records. Each record
has `role`, `provider`, positive unique integer `pid`, `executionProvider`,
`physicalDeviceUuid`, and `cudaVisibleDevices`. Identities are the fixed Tiger
`/example/provider/<role>` identities. Compute roles report
`CUDAExecutionProvider`, the same physical GPU UUID, and visibility `0`;
Merge reports `native-yolo-postprocess` and empty device/visibility strings.
Empty visibility explicitly hides CUDA even when the parent has an allocation.

These are validation requirements, not proof that a producer exists. T013
must still produce these records from real process/runtime/numerical evidence,
invoke the validator in the terminal path, and demonstrate mutation rejection.
Synthetic focused-test fixtures are never qualification evidence. Earlier
two-request/three-device fixtures are obsolete and must be rejected, not relabeled.
