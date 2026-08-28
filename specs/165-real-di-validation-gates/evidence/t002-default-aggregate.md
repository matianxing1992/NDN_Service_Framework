# T002 Default Aggregate Evidence

Status: PASS.

`Experiments/NDNSF_DI_Run_Local_Deployment_Gates.py` defaults to all four
mandatory cases. Legacy quick checks declare their fidelity and remain
non-authorizing. A passing diagnostic subset is explicitly prevented from
setting `externalValidationAuthorized`.

The closure aggregate contains Gate A, B, C, and D, with `passCount: 4`,
`failCount: 0`, `passed: true`, and `externalValidationAuthorized: true`.
Every run, including failed diagnostics, records `tigerClusterSubmitted:
false`.
