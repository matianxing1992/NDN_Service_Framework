# Spec175 M01 streamed-mode correction (2026-08-27)

The current source was run through the real four-Provider MiniNDN entry point
with `M01`, seed `1750001`, the checked-in tiny causal ONNX fixture, V3
automatic planning, one Request, and eight expected token events.  The run
returned `PASS` with four Provider handler spans, eight ordered events, the
exact token oracle `4,5,6,7,8,9,10,2`, and user return code `0`.

The run exposed and verified a source correction: the streamed application
context and terminal response now both carry `outputMode`/`generationMode` as
`TOKEN_STREAMING`.  Before the correction, the same event-producing path
reported `generationMode=FULL`, which was a semantic evidence mismatch rather
than a transport failure.

Run evidence (kept outside the source seal):

```text
caseResult=/tmp/spec175-m01-streamfix-G1JLqJ/spec175-case-result.json
caseResultSha256=sha256:6cdf6577992a7430abbfaa33805c5e1367960c73476072148bfbfa7726c26ea1
runLog=/tmp/spec175-m01-streamfix-G1JLqJ/run.log
runLogSha256=sha256:887664be7fdc6b60e55102909f54347dfed000f68cd5c885a9cf66d38cf888a1
providerCount=4
providerTimingSpanCount=4
events=8
generatedTokenIds=4,5,6,7,8,9,10,2
status=PASS
```

This is current host/tiny-ONNX process evidence.  It does not qualify the
Qwen3.6-27B model, CUDA residency, an exact SIF, or Tiger execution, and it
does not replace the repeated G2/T020 matrix.
