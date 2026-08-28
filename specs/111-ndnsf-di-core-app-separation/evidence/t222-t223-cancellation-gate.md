# T222-T223 Distributed Cancellation Gate

Date: 2026-07-15  
Verdict: **PASS**

## Candidate and scope

- Source baseline: `4d695ce8b7ffe2c79465dc1f3db649a5a65806a6`.
- Cancellation candidate: `spec111-cancel-4d695ce8b7ff-447e2b609c5f`.
- MiniNDN only; no Docker, OCI/SIF, iTiger, Slurm or host/default NFD was used.
- The MiniNDN dummy-keychain patch means this gate proves the authenticated
  evidence wiring and attempt/member state machine, not production key strength.

## T222 focused implementation gate

`InferenceRequestHandle` retains the exact execution certificate wire and
digest. `APPClient.cancel()` reconstructs that certificate, sends one bounded
control to every unique certified Provider through the existing collaboration
service, verifies Provider/service/requester/request/attempt/certificate/
cancellation bindings and authenticated Data evidence, and durably records the
terminal state only after all expected evidence is available. A process-local
`Future.cancel()` is only best-effort cleanup after that commit.

Commands and results:

```bash
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 -m unittest discover -s tests/python \
  -p 'test_ndnsf_di_request_cancellation.py' -v
# 10/10 PASS

./waf build --targets=unit-tests
# PASS

build/unit-tests \
  --run_test=NativeProviderExecutionControlCancelsAndSupersedesInPayload \
  --log_level=all
# 1/1 PASS
```

The Python negatives cover missing evidence, stale attempts, wrong Provider,
wrong Provider signer, replayed cancellation identity and Provider rejection.
Restart idempotence and accepted-terminal preservation also pass.

## T223 MiniNDN fault gate

The first completed attempt is retained at
`results/spec111-t223-cancellation-4d695ce8b7ff-447e2b609c5f` as **invalid
evidence**, not a protocol pass. Its default cancellation delay was 9 seconds;
all deterministic stages had already completed and every Provider correctly
returned `CANCEL_REJECTED`. It was not rerun or overwritten.

The affected gate was corrected by holding each deterministic stage for five
seconds so the same 9-second cancellation occurs during execution:

```bash
sudo -n env -u NDN_LOG \
  PYTHONPATH="$PWD/NDNSF-DistributedInference:$PWD/pythonWrapper" \
  python3 Experiments/NDNSF_DI_NativeTracer_Minindn.py \
  --full-network --core-trace --tracer-deterministic-runner \
  --enable-execution-leases --requests 1 --concurrency 1 \
  --skip-provider-pair-telemetry-probe --role-execution-delay-ms 5000 \
  --spec111-fault post-certificate-cancellation \
  --out results/spec111-t223-cancellation-delay5s-4d695ce8b7ff-447e2b609c5f
```

Observed result:

- `status=SUCCESS`; cancellation gate schema status `PASS`.
- Exact certified set contained Backbone, Head/Shard/0, Head/Shard/1 and Merge;
  all four returned Provider-signed, wire-digested control evidence for the
  original request and attempt.
- The already completed Backbone rejected cancellation; the three still-active
  Providers accepted it. No cancelled attempt published an accepted result.
- All four Providers rejected stale attempt-1 cancellation after attempt 2 was
  installed, then all four accepted attempt-2 cleanup.
- A separate accepted terminal request remained accepted; all four late
  cancellations were rejected.
- `survivors=[]`; harness cleanup stopped all Provider/User/Controller/NFD
  processes and no launcher survived the gate.
- No performance matrix was started.

Evidence digests:

- `summary.json`: `a9c9870e32d442aad56bbb86dc8276bee23d5822638921938fc95dd34b4b9747`
- `summary.txt`: `37b2dcbc2d2b4703522ac637a52b109d4af3ff5fc7b790e758ef8bf2850ef2c4`
- `logs/user-driver.log`: `0499ed3cd43784bf13cd683c05f857e15653c44e311db8792377f7a99f0f471c`

## Rollback

The APP SDK control transport is additive. Rollback removes
`execution_control.py`, the certificate fields on `InferenceRequestHandle` and
the post-certificate branch in `APPClient.cancel()` together; retaining only
the process-local Future cancellation is not an admissible rollback because it
would recreate the audited false distributed-success claim.
