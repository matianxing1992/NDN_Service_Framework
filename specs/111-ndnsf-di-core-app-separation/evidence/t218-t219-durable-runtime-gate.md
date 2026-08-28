# T218-T219 Durable Runtime and Request Recovery Gate

Date: 2026-07-15  
Verdict: **PASS**  
Scope: local Python gates plus one focused MiniNDN Controller/Provider/User
workflow. No performance matrix, container runtime, Slurm or iTiger activity.

## RuntimeJournal contract

`RuntimeJournal` now provides typed failures for unsupported schema versions,
writer-lock contention, quota exhaustion and unsafe/read-only roots. Atomic
transactions validate both outer and nested record schemas. Retention compacts
expired request streams and superseded deployment/operation snapshots while
preserving live and unknown records. Protected envelopes use AES-256-GCM,
owner-only paths, `O_EXCL`/`O_NOFOLLOW`, atomic replacement and concurrent file
plus directory `fsync`; both durability barriers finish before `submit()` may
return.

Focused command:

```bash
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 tests/python/test_ndnsf_di_runtime_journal.py
```

Result after the initialization durability follow-up: **13/13 PASS** in 0.066
seconds. This covers restart/torn tail, corruption, outer and nested schema
rejection, lock contention, read-only state, quota, bounded compaction,
protected-envelope tamper/expiry/cleanup, exact 32-byte envelope-key validation,
first-creation key/directory durability barriers, incremental hot-path indexes
and atomic multi-record append.

## Durable request identity

The public durable handle is `InferenceRequestHandle`, with a protected
`RequestEnvelopeReference`, requester identity, attempt epoch, deployment
revision, intent/certificate/result-rendezvous identity, event cursor,
cancellation identity/reason and terminal evidence digest. Reopen and cancel
are attempt-fenced; identity, retained envelope and result-rendezvous mismatch
fail closed. Network results retain Provider Data name, signer certificate and
wire digest.

Focused commands:

```bash
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 tests/python/test_ndnsf_di_request_handle.py
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 tests/python/test_ndnsf_di_app_sdk_compatibility.py
```

Results: **14/14 PASS** in 0.150 seconds and **8/8 PASS** in 0.393 seconds.

## Focused real-network recovery gate

The first focused attempt is retained at
`results/spec111-t219-network-gate-20260715T102851Z`. It reached the real
durable reopen and failed closed with `REQUESTER_IDENTITY_MISMATCH`. Diagnosis
showed that the LLM pipeline's restart caller constructed `APPClient` from the
correct journal namespace but did not pass the original
`/example/llm-pipeline/user` requester identity. The SDK check was correct; the
caller was fixed to preserve the original identity. No performance campaign
was restarted.

The affected focused gate was then rerun once at
`results/spec111-t219-network-gate-20260715T103103Z` using:

```bash
sudo -n -E env \
  PYTHONPATH="$PWD/pythonWrapper:$PWD/NDNSF-DistributedInference:$PWD/NDNSF-DistributedRepo/pythonWrapper" \
  LD_LIBRARY_PATH="$PWD/build" \
  timeout 300s python3 Experiments/NDNSF_DI_LlmPipeline_Minindn.py \
  --topology-file Experiments/Topology/AI_Lab.conf \
  --output-dir results/spec111-t219-network-gate-20260715T103103Z \
  --campaign-id spec111-t219-network-gate-20260715T103103Z \
  --runtime fake --stages 3 --layers 24 --compute-delay-ms 1 \
  --warmup-requests 0 --measured-requests 1 --measured-duration-s 0 \
  --request-interval-ms 0 --max-new-tokens 1 \
  --ack-timeout-ms 1500 --timeout-ms 60000 \
  --ndn-log 'ndn_service_framework.*=WARN' --deployment-workflow \
  --app-state-root /tmp/spec111-t219-network-gate-20260715T103103Z-state
```

Result: **PASS**, exit 0. The real ServiceController, three Providers and User
completed prepare/activate, one durable distributed request, APPClient restart
and reopen, drain, rollback epoch 2 and final delete. The verifier observed 22
unique wire request IDs and terminal deployment state `DELETED`.

- workflow summary SHA-256:
  `b8a7b68d9f30e2d10d5cb6c05ccb32f0d136895210be0d2814a98097f33d2357`;
- User log SHA-256:
  `dbb51cf0aba3633152f0a650a47d06b4146d99e6008addb8a05371fc2d9d244a`;
- no Controller, Provider, User, NFD or MiniNDN process survived cleanup;
- temporary state roots were removed after evidence verification;
- the failed attempt and successful result logs remain immutable evidence.

T218 and T219 are therefore closed. T220 may proceed from its local
microbenchmark and one isolated treatment diagnostic; neither result permits
restarting any earlier matrix.
