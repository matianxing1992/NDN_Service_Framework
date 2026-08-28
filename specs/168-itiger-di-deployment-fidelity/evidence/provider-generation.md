# T007 Provider Preparation and Complete-Generation Evidence

Date: 2026-08-03

Status: **T007 IMPLEMENTATION COMPLETE; GATE B/C ADMISSION PENDING**

T007 is complete at its implementation boundary. The source-level, native,
container-compatibility, and focused runtime contracts pass. This does not
authorize T008: the development host has neither `nvidia-smi` nor `apptainer`,
and no real MiniNDN campaign, exact-SIF CUDA preflight, model preparation,
container rebuild, SIF materialization, or TigerCluster inference job was
started while collecting this evidence.

## Implemented invariants

- A V2 assignment seals exact Provider backend and device from the signed ACK
  offer through final Selection and Provider preparation.
- Provider execution cannot publish `LOCAL_READY` until the assigned artifact
  is verified and the adapter reports load plus a real warmup on the exact
  assigned device with zero CPU fallback.
- Python and native paths fail closed on missing or mismatched runtime evidence,
  backend, device, role, artifact digest, load, warmup, or CPU fallback.
- Qwen stage artifacts are SHA-256 verified as a stream. Runtime preparation no
  longer calls `Path.read_bytes()` and therefore cannot allocate a second
  multi-gigabyte in-memory copy merely to verify a shard.
- The default generation path accepts one prompt input, performs its bounded
  token loop inside the committed collaboration, records ordered token evidence,
  and publishes one exactly-once complete terminal Response.
- The Gate B/C analyzer now requires three exact, explicitly classified device
  assignments and binds every adapter-confirmed loaded/warmed artifact digest to
  the digest delivered through DistributedRepo.
- The analyzer now distinguishes explicit `CPU_LOGIC` local evidence from
  `CUDA` acceptance. Deliberately assigned CPU execution with zero fallback may
  validate MiniNDN/container logic, but `--require-cuda` rejects it and remains
  mandatory for the exact-SIF GPU preflight and all TigerCluster acceptance.

## Focused verification

```text
python3 tests/python/test_spec168_provider_generation.py
Ran 7 tests - OK

python3 tests/python/test_spec161_qwen_generation.py
Ran 14 tests - OK

python3 tests/python/test_spec162_qwen36_repo_prepare.py
Ran 1 test - OK

python3 tests/python/test_ndnsf_di_selection_dataflow.py
Ran 21 tests - OK

python3 tests/python/test_ndnsf_di_placement_strategy.py
Ran 7 tests - OK

python3 tests/python/test_ndnsf_di_presplit_first_strategy.py
Ran 7 tests - OK

python3 tests/python/test_ndnsf_di_automatic_collaboration_plan.py
Ran 14 tests - OK

python3 tests/python/test_ndnsf_di_lifecycle_history.py
Ran 3 tests - OK

PYTHONPATH=.:NDNSF-DistributedInference \
  python3 tests/python/test_spec168_real_minindn_gate.py
Ran 8 tests - OK

build/unit-tests \
  --run_test=ExecutionEvidenceRoundTripsAndExcludesSecrets,\
NativeProviderRuntimeReadinessRequiresExactCudaLoadAndWarmup
Ran 2 tests - no errors

PATH=/usr/bin:/bin ./waf build --targets=unit-tests -j4
build finished successfully

PATH=/usr/bin:/bin ./waf build --targets=di-native-provider -j4
build finished successfully

python3 -m py_compile <four changed Python files>
git diff --check -- NDNSF-DistributedInference tests/python \
  specs/168-itiger-di-deployment-fidelity/jobs
PASS

docker run --rm --memory=4g --memory-swap=5g \
  --entrypoint /bin/bash \
  -v /home/tianxing/NDN/ndn-service-framework:/workspace:ro \
  -e PYTHONPATH=/workspace:/workspace/NDNSF-DistributedInference:\
/opt/ndnsf-app/python \
  ndnsf-di:spec162-fix019-repo-binding \
  -lc '<provider-generation test> && <real-MiniNDN-gate contract test>'
Ran 7 + 7 tests - OK
```

The first read-only container attempt omitted the image's
`/opt/ndnsf-app/python` binding path and correctly failed at import with
`ModuleNotFoundError: No module named 'ndnsf'`. The corrected command above
uses the same existing image and no rebuild; it is a container compatibility
gate, not an exact-SIF or physical-CUDA acceptance result.

The complete 443-case native suite reached one deterministic failure in the
unrelated `StreamFacade/PredictiveConsumerValidatesAndDeliversReorderedData`
test (`futureCursorHorizon 4 != 8`). A direct rerun reproduced that same Stream
assertion. The focused DI runtime tests and both native DI builds pass; this
document does not relabel or repair the unrelated Stream failure.

## Remaining admission evidence

T007 completion does not promote mocks or unit evidence into deployment proof.
Before T008, the next candidate must emit a real
`ndnsf-di.spec168-runtime-admission.v1` manifest proving:

1. independent Controller, Repository, User, and three Provider processes under
   real MiniNDN/NFD;
2. actual model bytes delivered through DistributedRepo without shared-path
   Provider injection;
3. three adapter assignments with exact CUDA devices, verified artifact
   digests, completed load and warmup, and zero CPU fallback;
4. one request ID, one wire Request, zero token Requests, ordered multi-token
   evidence, and one authenticated complete Response;
5. the same behavior inside the digest-bound candidate SIF.

Failure at either admission gate remains a BLOCK and cannot authorize T008.
