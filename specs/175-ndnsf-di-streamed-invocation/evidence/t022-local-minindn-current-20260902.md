# Spec175 T022 current CPU/MiniNDN production-path closure

**Run date:** 2026-09-02  
**Source identity:** `evidence/t022-source-seal-current-20260902-r1.json`  
**Runtime:** current working tree, real MiniNDN/NFD/NDN-SVS, tiny ONNX CPU
reference, admission control disabled  
**Scope:** M01, M11, M12, M13, and M14 representative production-path cases;
these runs do not claim Tiger, SIF, GPU, latency, or performance qualification.

## Commands

Each case was launched as a separate supervised process with the same wrapper:

```text
sudo -n env SPEC175_RUN_REAL_MININDN=1 PYTHONPATH="$PYTHONPATH" \
  python3 Experiments/NDNSF_DI_StreamedGeneration_Minindn.py \
  --case <M01|M11|M12|M13|M14> --seed <registered-seed> \
  --output-dir results/spec175/t022-current-<case>-<seed>-sudo
```

Registered run directories and outcomes:

| Case | Seed | Result | Production-path evidence |
|---|---:|---|---|
| M01 | 175021 | PASS | 8 ordered tokens, one terminal Response, ACK/Selection/Response, cleanup |
| M11 | 175022 | PASS | two fresh requests, four conversation hits, terminal Responses |
| M12 | 175024 | PASS | three conversations, six fresh requests, twelve hits, host prefetch/pause transitions |
| M13 | 175023 | PASS | restart/mismatch negative, one explicit full-prefill fallback, terminal Responses |
| M14 | 175025 | PASS | cancelled prefetch, successor checkpoint epoch, winner Response, terminal cleanup |

Every `spec175-case-result.json` reports `status=PASS`, no unexpected signal
exit, and no surviving owned process. Expected teardown signals are recorded
as intentional in M14's terminal evidence; the cancelled prefetch is an
expected negative boundary, not a failed generation.

## Marker summary

- M01: `LLM_PIPELINE_TINY_ONNX_STREAM ... tokens=4,5,6,7,8,9,10,2`.
- M11: `networkRequests=2 freshRequests=2 freshGenerations=2 conversationHits=4`.
- M12: `networkRequests=6 freshRequests=6 freshGenerations=6 conversationHits=12`.
- M13: `networkRequests=3 freshRequests=3 providerRestartCount=4 fullPrefillFallbackCount=1 preNetworkNegativeChecks=1`.
- M14: `networkRequests=3 freshRequests=3 cancelledPrefetches=1 successorCheckpointEpoch=2`.

## Boundary and qualification status

This closes the current focused T022 production-path evidence for the listed
cases under the sealed source identity. The earlier Artifact STORE segmentation
fault remains preserved as historical negative evidence; it is not silently
relabelled as a pass. T023 still must run the repository-owned contract/source
closure, verify all required T022/T023 evidence and hashes, and produce the
immutable `LOCAL_FUNCTIONAL_PASS` handoff before Spec180 can use Q-C/Q-W as
formal inputs. No SIF or Tiger action is authorized by this document alone.
