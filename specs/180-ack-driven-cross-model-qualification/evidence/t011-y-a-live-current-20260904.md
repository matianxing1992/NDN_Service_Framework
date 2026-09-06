# T011 — Current-source real-NFD Y-A wiring evidence (2026-09-04)

**Status**: `SPEC180_CASE_RESULT status=PASS case=Y-A`

This is focused current-source T011 production-wiring evidence, not the T014
design-code convergence verdict, T015 local qualification, exact-SIF evidence,
or Tiger evidence. It was executed after the revision-123 readiness,
canonical terminal-output, readiness-cancellation, and Spec180 marker fixes.
SIF/Tiger were not started.

## Run binding

- Case: `Y-A`; one `FullModel` Provider; one cold request.
- Candidate-bound package: `.codex-tmp/spec180-yolo-candidate-current`.
- Package candidate digest from the candidate-bound trust root:
  `sha256:35581dc38ed688a9c0e76c0fb173708361425ae1a64e86e64a38d8f23e1f61ef`.
- Candidate-bound trust root:
  `.codex-tmp/spec180-yolo-y-a-inputs-current-20260904/offer-trust-root.json`
  (SHA-256
  `sha256:9ce9d1817d4ab4e8d925f1ca57f0da27639ac3fd1ecb509e820efe331055b64d`).
- Signed runtime catalogue registry:
  `specs/180-ack-driven-cross-model-qualification/contracts/trust-root-registry-v1.json`
  (SHA-256
  `sha256:095fd99c8e008b1e683eaf67bc26efe665bc6c27e93cc802888e8b5eacca5219`).
- Fresh case output:
  `.codex-tmp/spec180-y-a-output-current-final2-FYC3jt`.
- Fresh state root:
  `.codex-tmp/spec180-y-a-state-current-final2-pXpm9k`.

## Executed path

Controller PUBPARAMS readiness completed before the controller marker and
catalogue publication. The output log contains:

```text
ServiceController listening
SPEC180_CONTROLLER_READY
SPEC180_RUNTIME_CATALOGUE_PUBLISHED
```

The maintained User then recorded the ordered lifecycle milestones:

```text
INPUT_REFERENCE_PUBLISHED
REQUEST_SENT
ACK_CLOSED
GRAPH_READY
PLACEMENT_DECISION
ARTIFACTS_READY
PLAN_SEALED
SELECTION_COMMITTED
PROVIDER_EXECUTION_STARTED
TERMINAL_RESPONSE
```

The authenticated ACK/Selection path selected one V3 `FullModel` role:

- request: `/spec180-y-a-2983042b8c5fdbbb`
- ACK closure: `ackCount=1`
- selected runtime candidate digest:
  `sha256:1f678384d527b025e552de1ab614ab5953df0694aeb4d1e719ece9de22817d1b`
- selected runtime plan digest:
  `sha256:63ba06393ac11fc3615dd067ae2a41a9db21ab143da0680ab7936826aa773bd2`

The Provider emitted real execution evidence:

```text
runnerKind=onnxruntime-cpu
realCompute=true
device=cpu0
runtimeVersion=1.26.0
cpuFallbackUsed=false
loadCompleted=true
warmupCompleted=true
```

It recorded model digest
`sha256:95921f88427a722bc56437b809fa6a82fadfa3020a5374d75761289131293539`
and artifact digest
`sha256:fa045a9525bf6cb8a4695ceff0b6c34892a925a956620776e1e3110c2718bdc3`.
The terminal oracle was:

```text
YOLO_ACK_DRIVEN_RESULT status=true payload_bytes=7267 plan_digest=sha256:f5ccd1e941991503f7ec232e3d453dbbf247902ba8ad290058e6b1b372ad5241
```

No `DI_FINAL_RESPONSE_MISSING` or `DI_RUNTIME_EVIDENCE_MISSING` occurred, and
the supervised run left no Controller, Provider, User, Repository, or NFD
process alive.

## Runtime/build identities

- `build-system-j2/examples/di-native-provider`:
  `sha256:4907a3675852ee72a4956442d930ba958608051df25eb5854512ea9d065e07c7`
- `build-system-j2/libndn-service-framework.so`:
  `sha256:3aaaa745959aa11ab0f9952b61fcbbe369c32f0d4afb4ccbe0cdd929b13829ef`
- `pythonWrapper/ndnsf/_ndnsf.cpython-38-x86_64-linux-gnu.so`:
  `sha256:af94a044f8c83cd2b7aeea14962affa2e0ca5895e72f850c0c2da9ecfc321f90`
- Python import smoke: `ndnsf._ndnsf` and
  `ndnsf_distributed_inference` — `PASS`.
- Native closure observation: Python extension and NFD resolve
  `libndn-cxx.so.0.9.0` to `/usr/local/lib/libndn-cxx.so.0.9.0`; NDN-SVS
  resolves to `/home/tianxing/NDN/ndn-svs/build/libndn-svs.so.0.1.0` and NAC-ABE
  to `/usr/local/lib/libnac-abe.so`.

The run is retained as source-bound focused evidence only. T014 must still
reinspect the complete source/design/evidence graph and return a fresh PASS
before T015 or any SIF/Tiger operation.
