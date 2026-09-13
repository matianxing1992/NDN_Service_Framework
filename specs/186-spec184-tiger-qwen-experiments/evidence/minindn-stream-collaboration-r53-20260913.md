# Spec186 local streamed collaboration repair — r53

**Date:** 2026-09-13
**Run:** `spec186-host-gate-20260913-r53`
**Scenario:** real MiniNDN M01 host gate, four native tiny-ONNX providers,
  V3 deferred placement, two generated tokens (`4,5`).
**Qualification scope:** framework/collaboration regression only. This is not
  a YOLO Y-A/Y-B result and does not close T007.

## Command and identity

```text
sudo -n env NDNSF_SVS_DIAGNOSTIC=1 NDNSF_RESPONSE_LARGE_DATA_THRESHOLD=8192 \
  NDNSF_SVS_PUBLICATION_FETCH_RETRIES=3 \
  NDNSF_SVS_PUBLICATION_FETCH_INNER_RETRIES=2 \
  NDNSF_SVS_PUBLICATION_FETCH_LIFETIME_MS=1000 \
  NDNSF_SVS_PUBLICATION_FETCH_MAX_BACKOFF_MS=4000 \
  NDNSF_REINIT_UNREGISTER_SETTLE_MS=150 \
  python3 Experiments/NDNSF_DI_LlmPipeline_Minindn.py \
  --topology-file Experiments/Topology/spec175-host-gate.conf \
  --spec175-case M01 --seed 18653 --initial-sync-settle-s 5 \
  --selection-dataflow-v3 --request-id /spec186-r53 --max-new-tokens 2 \
  --stages 4 --runtime tiny-onnx --ack-timeout-ms 10000 \
  --timeout-ms 120000 \
  --output-dir Experiments/TigerCluster/results/spec186-host-gate-20260913-r53 \
  --minindn-root /tmp/spec186-minindn-r53 --nlsr-wait-s 12 \
  --provider-start-timeout-s 180
```

The placement candidate digest was
`sha256:85b150ba10b1b948c599ad4d4fe7e910050e580c968e4792852b474f6bf84057`.
The committed plan digest was
`sha256:625ba5eeb964cd81558367dbe21f4b3d904d5b48d196947640332c628d9a62d6`.
The source patch was built from the Spec186 working tree immediately before
the checkpoint commit that records this receipt; the later candidate source
seal must therefore be regenerated before any SIF or Tiger promotion.

## Acceptance evidence

| Boundary | Evidence |
| --- | --- |
| Current request reached every Provider | `stage0`–`stage3-provider.log` each contain `NDNSF_SVS_REQUEST_SEEN ... /spec186-r53` and `NDNSF_DI_ACK_DECISION ... status=true` |
| Non-terminal stream handling | Stage/0, Stage/1 and Stage/2 contain `NDNSF_STREAM_GRANT_SKIP_NONTERMINAL`; each continues with `LLM_PIPELINE_QWEN_FULL_HIDDEN_*` and token forwarding |
| Terminal grant binding | User log contains `NDNSF_STREAM_GRANT_BUILT ... provider/3 grantBytes=366`; Stage/3 contains `NDNSF_STREAM_GRANT_ACCEPTED` |
| Dependency execution | Stage/1–3 contain hidden activation receive/publish events; Stage/0 consumes token feedback and Stage/3 publishes tokens |
| Numerical/token oracle | User log: `LLM_PIPELINE_TINY_ONNX_STREAM ... events=2 tokens=4,5 retries=6 duplicates=0`; terminal response has `generatedTokenIds=[4,5]`, `stopReason=TOKEN_LIMIT`, `stageCount=4` |
| Process and cleanup | `NDNSF_DI_SPEC175_CASE_PASS case=M01` and `LLM_PIPELINE_MININDN_OK`; MiniNDN stops controller, links, switches and all eight hosts |

## Root cause and correction

The compact multi-Provider Selection path had no per-Provider stream-grant
container. After it was split into per-Provider Selection messages, the
request-scoped Provider path still unconditionally initialized a stream
publisher for every collaboration role. Only the terminal role is authorized
to receive the event key, so non-terminal roles were incorrectly turned into
`request-scoped stream grant rejected` failures. The Provider now initializes
the publisher only when a grant exists, and permits a missing grant only for a
registered collaboration service with an assignment payload. Ordinary
streaming requests remain fail-closed.

## Boundary

This receipt demonstrates a real MiniNDN multi-Provider stream and fixes the
previous application-level blocker. It does not provide YOLO Y-A/Y-B/Y-N,
exact source-sealed base SIF, Qwen3, CUDA, Tiger single-node, Tiger two-node,
or reuse evidence.
