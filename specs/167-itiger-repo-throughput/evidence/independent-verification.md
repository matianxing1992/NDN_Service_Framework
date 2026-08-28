# Spec 167 source-013 independent verification

The promoted source-013 evidence was independently re-analyzed locally from
the copied `campaign-manifest.json` and `run-records.jsonl`; the remote
`derived-results.json` was not treated as the only authority.

```bash
python3 Experiments/analyze_spec167_itiger_repo.py \
  --manifest results/spec167-source-013-freeze/remote-evidence/campaign-manifest.json \
  --runs results/spec167-source-013-freeze/remote-evidence/run-records.jsonl \
  --output /tmp/spec167-independent-013.json
```

The independent analyzer emitted `SPEC167_ANALYSIS_PASS` and reproduced:

| Check | Result |
|---|---:|
| Campaign identity | `spec167-tiger-20260802-source013` |
| Expected / observed rows | 60 / 60 |
| Warmups / measured | 10 / 50 |
| Measured failures | 0 |
| Missing / duplicate / unexpected | 0 / 0 / 0 |
| Path violations | 0 |

An independent JSONL pass over all 60 records also found 60 `PASS` statuses,
five subjects at each of two payload sizes, zero timeout and retransmission
counts, zero aggregate duplicate payload bytes, and no cold logical-byte
mismatch. The remote `checksums.sha256` was verified on TigerCluster before
promotion and again in the local mirror. The SIF digest and source checksum
logs are retained beside the evidence.

The verification is sufficient for the throughput, count, integrity, and
claim-boundary results above. It does not manufacture CPU, memory, wire-byte,
or full amplification metrics absent from the retained result schema; those
remain an explicit follow-up instrumentation gap.

