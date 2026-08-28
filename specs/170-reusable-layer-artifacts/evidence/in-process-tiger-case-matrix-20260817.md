# In-process matrix required before TigerCluster (2026-08-17)

This matrix is the pre-dispatch gate for the Spec170 Tiger cases. It verifies
the request/Selection/dataflow contracts locally before any SIF is submitted;
it does not replace Tiger hardware, CUDA, Slurm, or process-isolation evidence.

| Tiger case | Local integrated case | Result | Scope |
|---|---|---|---|
| D0, four Providers / one role each | `Spec170NdnsfDiCoreFlow/ProductionIngressRunsFourProviderRoleSplitRequestSelectionResponse` + `PreconfiguredEnvironmentRunsFourProviderRoleSplitCollaboration` | PASS | The production gate drives encrypted Request, per-Provider ACK, per-Provider Selection assignment, four handlers, and four Response packets; the companion retains the existing role-split lifecycle assertions |
| D1, one Provider / four roles | `Spec170NativePostSelection/ProductionIngressRunsNativePostSelectionAssignmentFetch` + `Spec170NativePostSelection/ProductionIngressRunsNativeFourRoleTwoDeviceAssignmentRequestResponse` + `Spec170NdnsfDiCoreFlow/ProductionIngressRunsEndToEndSelectionAssignmentResponse` + `PreconfiguredEnvironmentRunsSameProviderMultiRoleCollaboration` | PASS | The native gate delivers a segmented assignment through real SVS subscriptions, invokes `NativeProviderHandler`, and observes final Response publication; the broader projection gate remains a companion |
| D2a, one Provider / two devices | `Spec170NativePostSelection/ProductionIngressRunsNativeFourRoleTwoDeviceAssignmentRequestResponse` + `Spec170NdnsfDiCoreFlow/ProductionIngressRunsD2aAssignmentIntoTwoDeviceRuntime` + `Spec170IntegratedFlowsTest.test_d2a_two_device_roles_and_unsplittable_rejection` | PASS | Real SVS Request/ACK/Selection ingress, segmented assignment fetch, four-role dispatch across two local devices, and final Response publication; CPU structural gate, while GPU execution remains a Tiger requirement |
| D2b, two Providers / one logical two-rank group | `Spec170NdnsfDiCoreFlow/ProductionIngressRunsD2bSelectionIntoSvsDataV1` + `NdnsfDataV1SvsFlow/ProductionProviderContextUsesSvsSegments` and `IndependentSegmentsUseSvsMappingRepairAndReplayFence` | PASS | Real Selection ingress into Provider context, authenticated `NDNSF_DATA_V1` SVS segment publication/fetch, out-of-order delivery, duplicate/replay fence, and Provider context fetch. CPU/wire gate; cross-process Tiger evidence remains required |
| D2h, heterogeneous `[1,2,1]` and `[2,1,2]` | `Spec170NdnsfDiCoreFlow/ProductionIngressRunsD2hFrozenHeterogeneousMappings` + `Spec170IntegratedFlowsTest.test_heterogeneous_profiles_cover_both_frozen_d2h_mappings` and `HybridExecutionContractTest` | PASS | Real Request/ACK/Selection ingress for both frozen profiles, exact role-to-Provider mapping, local runtime dispatch, declared redistribution edges, and no phantom ranks. CPU structural gate; heterogeneous Tiger execution remains required |

## Commands and results

Native request/dataflow target:

```text
./waf build --targets=integration-tests -j2
build/integration-tests --log_level=test_suite --report_level=short
```

Result: the focused native suite currently passes **4/4 cases**, including a
real four-edge dataflow positive and a missing-Backbone-output terminal
negative. This includes the explicitly named `Spec170NativePostSelection/*`
cases; they are now in a
dedicated suite so a `Spec170NdnsfDiCoreFlow/*` filter cannot silently omit the
post-Selection native gate.

The existing focused D2a, D2b, and D2h structural gates remain companion
coverage; they cannot replace the new dependency-edge oracle.

Spec170 Python contract suite:

```text
PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper \
  python3 -m pytest -q tests/python/test_spec170_*.py
```

Result: **59 passed, 5 skipped, 1 warning**. The focused cross-module file
passed **7/7**.

Deployment-faithful local NativeTracer gate (real MiniNDN processes, real
ServiceUser/ServiceProvider callbacks, and the configured CPU ORT ABI):

```text
SPEC170_RUN_REAL_NATIVE_MININDN=1 \
  PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper \
  pytest -q tests/python/test_spec170_real_minindn_gate.py -k native_tracer
```

Result: **2 passed** in 69.10 s. The two cases are `default` (D0, four
Providers/one role each) and `single-provider` (D1, one Provider/four roles).
Each case requires ACK matching, four assignment selections, provider
`SELECTION_RECEIVED`, provider execution, all four planned dependency edges
with matching producer/consumer Data names and non-empty payloads, and a final
User response. The gate
resolves the ORT library from `SPEC170_ORT_LIBRARY_PATH` or the current
`build/config.log`, so a host ORT ABI mismatch fails before the workload can be
reported as a pass. It intentionally does not use the synthetic deterministic
runner.

For a promoted candidate, run both the exact-SIF CLI closure and the real
exact-SIF network closures before submission:

```text
SPEC170_EXACT_SIF=/path/to/runtime.sif \
SPEC170_EXACT_SIF_BUNDLE=/path/to/read-only-bundle \
  pytest -q tests/python/test_spec170_real_minindn_gate.py -k exact_sif
```

This checks the complete Provider command surface and then launches real D0
four-Provider and CPU single-Provider/four-role Controller/User/Provider
workloads entirely inside the exact SIF. Both network cases require explicit
Selection receipt, 4/4 dependency publication/fetch evidence, and a final
Response; a host build pass or exact-SIF CLI/import pass alone is insufficient.
The current r10 diagnostic passed both exact-SIF network cases in **23.13 s**.

## Boundary of this evidence

The matrix proves the local protocol, assignment, scheduler, and authenticated
data-plane paths for every currently planned Tiger case. It does not claim that
the current SIF has passed D0/D1/D2a/D2b/D2h on TigerCluster. Before submission,
the exact candidate SIF and workload bundle still require the local SIF closure
gate, then each Tiger job must retain
its own signed offer, device map, runtime/provider traces, complete response,
and negative evidence. The previous candidate's D0 job `199484` is explicitly
classified `INVALID_CANDIDATE` because its embedded Provider did not support a
workload CLI option; see `tiger-d0-199484-stale-sif-cli-20260817.md`. A new SIF
identity is required before remote execution.
