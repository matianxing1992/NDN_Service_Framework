# T022-B Provider readiness and log-ordering correction (2026-08-30)

## Finding

The fresh host matrix `results/spec175/g3/current-20260830i` stopped at M14-r1.
The User received ACKs from only three Providers; Provider 1 had not received
the request.  The merged provider log appeared to put
`LLM_PIPELINE_PROVIDER_READY` before the native registration messages.  That
ordering is not a reliable proof: the Python readiness marker is written to
stdout while ndn-cxx registration logs use a separate stream, so line order in
a merged file can be reversed by buffering.  Source inspection confirms that
`NativeServiceProvider.start()` calls `ServiceProvider::init()` synchronously;
`init()` registers service information and NDNSF/SVS filters before the native
event thread is created and `start()` returns.  The M14 result therefore
identified a real three-Provider/ACK readiness symptom, but the old log alone
did not establish an early marker as its cause.

## Correction

`ServiceProvider.start()` remains the synchronous readiness seam.  It registers
the requested handlers idempotently and then calls the native provider start;
the distributed-inference facades forward this method.  The pipeline launcher
calls `provider.start()` before emitting `LLM_PIPELINE_PROVIDER_READY` and only
then enters the compatibility `run()` loop.  The marker is consequently a
call-return barrier, not a timestamp comparison between two output streams.
Future qualification evidence must use this explicit barrier (or separate
machine-readable readiness records); it must not infer readiness from merged
stdout/stderr line order.

The repository route probe remains bounded and non-mutating.  A probe may use
at most two retries after the initial attempt, each with a fresh request ID;
the final JSON retains every attempt and the publication barrier is released
only after a validated ACK.  A retry does not replace a failed qualification
repetition.

## Focused validation

- `pytest -q tests/python/test_streamed_invocation_api.py tests/python/test_spec175_real_minindn_gate.py`: 74 passed.
- M14 diagnostic `results/spec175/g3/diagnostic-M14-provider-start-20260830k/spec175-case-result.json`: `status=PASS`, `providerCount=4`, `campaignId=spec175-M14-1750001`, `userReturnCode=0`, and empty `survivingOwnedProcesses`.
- A fresh M04 diagnostic `results/spec175/g3/diagnostic-M04-provider-start-20260830m/spec175-case-result.json`: `status=PASS`, `providerCount=4`, `seed=1750001`, `userReturnCode=0`, empty `survivingOwnedProcesses`, and case-result SHA-256 `57a79cde614b1ad1f75403a7a523db2bbc40124784fc814003492ae612c03dbd`.
- The M04 run recorded `ackCount=4`, one wire Request, and a final Response with the eight-token oracle through the real four-Provider MiniNDN path.

These are diagnostic confirmations of the explicit startup barrier and the
four-Provider path; they do not prove that merged log lines are chronologically
ordered.  The source changed after the previous G0--G3/SIF identities, so this
does not close T022, T023, or any promotion gate.  A new source seal, G0--G2,
full fixed-seed 42-process G3 matrix, local SIF, and exact-SIF replay are
required.
