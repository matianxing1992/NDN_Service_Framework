# Spec189 r142 owned-swap resource boundary — 2026-09-20

## Scope

This record covers the fresh installed-binary MiniNDN run
`two-provider-global-r142`. It is a preserved runtime attempt after the r141
initial producer-readiness repair. No raw run directory was overwritten.

## Candidate and raw evidence

- run root: `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r142/`
- external Repo: `.codex-tmp/spec189-qwen-two-provider-20260918/external-r142/encrypted-repo/`
- launch output: `.codex-tmp/spec189-r142-launch.log`
- supervisor: `runs/two-provider-global-r142/supervisor.json`
- resource trace: `runs/two-provider-global-r142/resource-samples.jsonl`
- requester log: `runs/two-provider-global-r142/requester-0.log`
- Provider logs: `runs/two-provider-global-r142/provider-0.log` and
  `provider-1.log`

The run used the installed native binaries and a real five-node MiniNDN
topology. The host resource gate passed initially; cleanup after the stop was
`PASS` and no child process remained.

## Observed production progress

The run reached these durable boundaries:

1. both native Providers reached `NDNSF_DI_NATIVE_PROVIDER_READY`;
2. both Providers emitted signed `DI_PLACEMENT_V3_OFFER` ACK decisions;
3. both Providers emitted `NDNSF_DI_GRANT_VERIFICATION` with
   `status=VERIFIED` and `boundary=BEFORE_ASSEMBLY` for the same request and
   plan digest;
4. the external Repo contained both signed selection-assignment objects, and
   Provider-0 created an active assembly staging root.

The requester entered the native request route and later reported
`NATIVE_REQUEST_STAGE_FAILED code=CANCELLED boundary=request`. Provider-1 also
reported a socket EOF during shutdown. These are consequences of the
supervisor stop, not independent protocol failure results.

No `EXECUTION_ENTERED`, `DEPENDENCY_FETCH`, `RUNNER_READY`,
`EXECUTION_COMPLETED`, terminal response, numerical oracle, repeat request, or
qualification result was observed. Therefore this run is not a
`QWEN_TWO_PROVIDER_PASS`.

## First classified boundary

The supervisor stopped the run at:

```text
RESOURCE_BOUNDARY:ownedSwap
```

The guard limits were `maxOwnedSwapBytes=268435456` and
`maxSwapIoBytes=268435456`. The final running sample reached
`ownedSwapBytes=285523968`, while `availableBytes=7509356544`,
`diskFreeBytes=38231097344`, and `swapIoDeltaBytes=782553088`; cleanup then
drained the process group. This is a host resource boundary after grant
verification and before observable provider assembly/execution. It does not
establish a Repo, authorization, ORT, model-output, or qualification verdict.

## Task state and next gate

T003, T005, T006, T007, and T009 remain `PARTIAL`. The next real attempt must
use a new run ID and first pass the host resource gate under a lower paging
baseline or otherwise safe host state. It must then re-observe the missing
post-Selection sequence through terminal output before any qualification claim.
