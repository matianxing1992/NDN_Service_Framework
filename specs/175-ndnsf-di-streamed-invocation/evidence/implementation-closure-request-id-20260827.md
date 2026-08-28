# Spec175 implementation closure and Request-ID correction (2026-08-27)

## Audit correction

The prior task status made T014/T015/T030/T031 depend on the final repeated
G2/G3 campaign while T020/T022 were forbidden to run until those four tasks
closed. That was a circular dependency. The corrected ownership is:

- T014/T015 close the automatic streamed workload implementation and its
  focused real-process boundary;
- T030/T031 close the host/CPU conversation transaction and Provider-state
  implementation boundary;
- T020 owns the final same-source G0/G1/G2 repetitions;
- T022 owns the final same-source 42-process G3 matrix;
- T034 owns actual CUDA GPU-host-GPU residency and movement.

This separation does not weaken any qualification gate. It prevents repeated
qualification from being used as an implementation test and prevents the same
evidence from being required by two mutually blocking tasks.

## Request-ID defect and correction

The generic tiny-ONNX branch of
`examples/python/NDNSF-DistributedInference/llm_pipeline/user.py` accepted the
campaign Request ID but did not pass it to `_run_tiny_onnx_stream(...)`.
Consequently, an earlier result could label the campaign
`spec175-M01-1750001` while the real NDNSF Request used a generated
`/ndnsf-di-...` name. Those earlier M01--M07 results are diagnostic only for
request-lineage claims.

The branch now derives the bounded wire Request ID, passes it into the real
stream request, and fails if the returned Request ID differs. The focused
regression `test_tiny_stream_uses_and_verifies_the_campaign_request_id` covers
both forwarding and verification.

## Executed evidence

- Focused streaming/conversation/provider suite: **112 passed**.
- Post-fix real M01:
  `/tmp/spec175-request-id-fix-20260827/m01`
  - actual published Request ID: `/spec175-M01-1750001`;
  - streamed Request ID: `/spec175-M01-1750001`;
  - eight ordered token events: `4,5,6,7,8,9,10,2`;
  - one terminal response;
  - four Provider handler spans, zero open spans;
  - final case verdict: PASS.
- Current real M11:
  `/tmp/spec175-implementation-closure-20260827/m11`
  - four independent Provider processes;
  - two fresh network Requests and generation IDs;
  - four conversation hits;
  - delta-prefill token count 1;
  - committed conversation epochs 1 then 2;
  - zero fallback and zero state tensor bytes on NDN;
  - zero leaked request-local entries;
  - eight Provider spans, zero open spans;
  - final case verdict: PASS.

Source inspection and focused regressions additionally verify that Providers
publish signed/encrypted Ready and receipt Data, wait for exact User
COMMIT/ROLLBACK, commit staged promotion, and publish the Provider commit
acknowledgement.

## Evidence level and remaining work

This evidence establishes **implemented and host/CPU executed** boundaries for
T014, T015, T030, and T031. It does not establish the following:

- the complete 42-process M01--M14 matrix (T022);
- an exact final SIF or exact-SIF replay (T023);
- Qwen3.6 CUDA correctness/performance or GPU residency (T025/T034);
- Tiger G6/G7 execution (T026/T027).

T020 subsequently passed on the corrected subject: G0 reported zero blockers,
G1 reported 218 passed with zero failed/skipped, and G2 reported 38/38 results
covering I01--I20 with no missing case. T022's complete G3 matrix is therefore
the next valid action. SIF and Tiger remain strictly later gates.
