# Spec189 r140 MiniNDN exact-fetch boundary evidence

## Status

`PARTIAL` / real installed MiniNDN run with a classified Provider dependency
fetch failure. This record is not a `QWEN_TWO_PROVIDER_PASS`: no
`RUNNER_READY`, `EXECUTION_COMPLETED`, terminal response, numerical oracle,
repeat request, or qualification result was observed.

## Candidate and run identity

The run used the installed candidate from the preceding r140 focused build and
the unchanged Qwen candidate manifest. The raw run is retained at
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r140/`;
the launcher output is
`.codex-tmp/spec189-r140-launch.log`. The supervisor record is
`runs/two-provider-global-r140/supervisor.json`.

The run used topology `Experiments/Topology/AI_Lab.conf`, nodes `ucla` and
`arizona`, controller `memphis`, and the installed native requester/provider
targets. The stage manifest digest was
`sha256:4c90126bf974a91abeb555f57b25a786afcf68a11b45976f8a1d15ee1df9c097d`.

## Observed production boundary

The requester emitted `NDNSF_DI_NATIVE_ACK_CLOSED` and
`NDNSF_DI_NATIVE_SELECTION_COMMITTED`. Both Providers emitted
`NDNSF_DI_NATIVE_SELECTION_ACCEPTED` for the same plan digest and
`NDNSF_DI_GRANT_VERIFICATION` with `status=VERIFIED` and
`boundary=BEFORE_ASSEMBLY`.

Provider-0 then emitted `ASSEMBLY_ADMISSION_REPORTED`,
`EXECUTION_ENTERED`, and `ASSEMBLY_STARTED`. Provider-1 emitted the same
admission and execution events, then entered `DEPENDENCY_FETCH` for the
placement-bound tensor manifest published by Provider-0.

Provider-1 failed at the first unambiguous downstream boundary:

```text
Exact collaboration fetch failed ... attempts=356 error=deadline
NDNSF_DI_NATIVE_FAILURE ... failed to fetch signed exact Data: <Provider-0 tensor manifest>
NDNSF_DI_PROVIDER_STAGE stage=TERMINAL status=failed
```

The matching raw lines are in
`runs/two-provider-global-r140/provider-1.log`. Provider-0 had not emitted
`NDNSF_DI_EXACT_DATA_PUBLISHED` for this dependency before the run was stopped;
its log shows that it was still fetching/materializing large selected
materials. Therefore the evidence establishes a producer-readiness/deadline
boundary, not a missing-name, authorization, Repo, ORT, or numerical-result
verdict.

## Resource and cleanup boundary

The run was manually interrupted after Provider-1 reached the classified
failure while Provider-0 continued material transfer. The outer supervisor
therefore records `boundary=CANCELLED`, `returncode=-2`, and
`cleanup=PASS`; this is an operator-stop fact, not a successful protocol
completion. The final drained sample records no remaining processes, about
`41.52 GiB` free disk, about `9.82 GiB` available memory, and
`ownedSwapBytes=0`.

## Next gate

The next repair must review the production dependency deadline contract before
changing a timeout value: a fixed `no_progress_ms=180000` bound currently
expires while the upstream Provider is still assembling and before its exact
manifest publication. The repair must preserve a bounded hard deadline,
provide a progress/readiness basis for the initial dependency wait, pass the
affected C++ static/build/runtime gates, and use a new run ID. This r140 raw
run must not be overwritten or counted as a qualification attempt that passed.
