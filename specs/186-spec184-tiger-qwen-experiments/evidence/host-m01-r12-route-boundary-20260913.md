# Spec186 host M01 retry route boundary

The current-source host retry used the repaired native build, the unique
run-scoped `NDNSF_CONTROLLER_GENERATION_STATE`, and passwordless root MiniNet
ownership. This is a failed prerequisite run, not Spec186 qualification.

| Field | Value |
| --- | --- |
| run | `spec186-host-gate-20260913-r12/M01` |
| command | `sudo -n env ... SPEC175_RUN_REAL_MININDN=1 python3 Experiments/NDNSF_DI_StreamedGeneration_Minindn.py --case M01 --seed 1750001 ...` |
| runtime | local Apptainer 1.5.3 was available but no SIF was declared; native host libraries came from `build-spec186-r5` and the explicit NAC-ABE prefix |
| reached | MiniNet topology, NFD routes, controller startup, repository provider startup |
| first failure | repository publisher's three bounded `/NDNSF/DistributedRepo/Object/v1/STATUS` probes timed out |
| terminal | `REPO_SERVICE_ROUTE_NOT_READY`; no application request or numerical result |
| cleanup | MiniNet stopped controller, links and hosts; output retained under the run directory |

The preceding non-root retry `r10` stopped at MiniNet's root-owner check. The
ABI mismatch seen in older attempts is absent from r12; the controlling issue
is now the repository route/readiness barrier.
