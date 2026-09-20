# B189 r144 Static-Routing MiniNDN Owned-Swap Boundary

**Date**: 2026-09-20
**Status**: `PARTIAL`
**Run**: `two-provider-global-r144`

## Candidate and Changed gate

This was the first fresh real Qwen run after checkpoint `c03260d2`, using the
installed candidate and a new run/external root. The Changed gate was the
reviewed MiniNDN entry: `Nfd + NdnRoutingHelper` owns deterministic application
prefix routing, NLSR is not started, `--routing-wait-s` is used, and the
node-to-APP plan is validated and recorded. The static review returned
`STATIC_PASS` with no P0/P1/P2 finding.

The exact run used:

- topology: `Experiments/Topology/AI_Lab.conf`
- stage nodes: `ucla`, `arizona`
- Controller/Authority: `memphis`
- Requester: `neu`
- forwarder-only node: `wustl`
- installed binaries under `/usr/local/bin` and `/usr/local/libexec`
- raw launch log: `.codex-tmp/spec189-r144-launch.log`

Raw run evidence is retained at
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r144/`;
the supervisor record is `supervisor.json` and resource samples are
`resource-samples.jsonl`.

## Runtime boundary

Observed before the stop:

- all five NFD instances started;
- Controller and Authority started;
- both Provider instances reached `NDNSF_DI_NATIVE_PROVIDER_READY`;
- both Providers emitted signed `ACK_DECISION` offers;
- both Providers emitted `NDNSF_DI_GRANT_VERIFICATION` at
  `BEFORE_ASSEMBLY`;
- the NFD faces carried substantial request traffic, so the run was not a
  topology-construction-only failure.

The maintained host guard stopped the run at the first classified boundary:

- `RESOURCE_BOUNDARY:ownedSwap`;
- maximum `ownedSwapBytes=271720448` against the
  `268435456` limit;
- maximum `swapIoDeltaBytes=870912000` also exceeded its
  `268435456` limit, but the supervisor boundary remains the owned-swap
  boundary;
- minimum available memory was `5501882368` bytes;
- minimum disk free was `32500576256` bytes;
- cleanup was `PASS` and no child process remained;
- requester `CANCELLED` was shutdown consequence.

Not observed: requester `ACK_CLOSED`, `Selection`, `EXECUTION_ENTERED`,
`DEPENDENCY_FETCH`, `ASSEMBLY_STARTED`, `RUNNER_READY`,
`EXECUTION_COMPLETED`, terminal response, numerical oracle, repeat/drain and
qualification PASS.

## Five-lane disposition

- `static`: upstream MiniNDN routing examples, current entry, topology and
  node-to-APP plan were reviewed; read-only review-agent result was
  `STATIC_PASS`.
- `compile/link`: no C++ source changed and no rebuild was performed; the
  installed candidate identity remained bound by the launcher digests.
- `runtime/test`: Python syntax/help/node-plan checks and the five-node static
  route smoke passed. The real r144 run reached Provider READY/ACK/grant and
  then stopped at the host owned-swap boundary.
- `unobserved`: the corrected entry has not reached requester Selection,
  Provider assembly/execution, terminal output, numerical oracle, repeat or
  qualification.

No Spec189 task checkbox changes. T003, T005, T006, T007, and T009 remain
`PARTIAL`.
