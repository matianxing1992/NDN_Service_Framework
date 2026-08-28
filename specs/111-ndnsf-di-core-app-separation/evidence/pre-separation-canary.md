# Pre-Separation MiniNDN Canary

Verdict: **FAILED BEFORE MEASUREMENT — RETAINED, NO RERUN**  
Execution date: 2026-07-14  
Candidate: `spec111-pre-separation-canary`, seed `11100`  
Source base: `4d695ce8b7ffe2c79465dc1f3db649a5a65806a6`

## Once-only execution

The exact command is the pre-separation command frozen in
`performance-baseline-recipe.md`. System journal evidence records its one real
launch at `2026-07-14 15:40:52 -0500` as sudo PID 468433 and Python PID 468436.
The sudo session closed at `15:41:20`; it was not started a second time.

An earlier owner check returned `SPEC111_CANARY_REFUSED_LIVE_OWNER` because its
`pgrep -af` expression matched the current shell's command text. That check
exited before creating the output/log/status paths and before invoking sudo or
Python, so it is not a campaign cell. The corrected process-executable check
then launched the one real cell.

## Observed result

- Policy generation completed for the fake three-stage pipeline.
- MiniNDN reached `Creating network` and `Adding controller`.
- Root and node certificates were created.
- No controller/provider/request process evidence, warmup result, measured
  request, summary, p50/p95, throughput or completion metric was emitted.
- The expected status sidecar was not written because MiniNDN global cleanup
  matched and terminated the supervising shell whose command text contained
  the harness name. This is the retained harness/supervision failure; no exit
  code is invented.
- Command log:
  `results/spec111-core-app-separation/pre-separation-canary-command.log`,
  SHA-256 `78776959d9db0e9fbf6598b30ce9d87c99a28170230d0fde9383da57265b501b`.
- Partial output:
  `results/spec111-core-app-separation/pre-separation-canary/`.

The failure is not admissible latency evidence and cannot establish a
pre-separation performance value. It is admissible failed-cell evidence under
the no-replacement rule. The final matched campaign must use the immutable
pre-movement source snapshot as baseline and first fix supervision so the
launcher cannot kill its parent; this failed cell itself remains unchanged.

## Cleanup

Generated private `*.ndnkey` and request `*.req` files were deleted rather than
retained as evidence. Public certificates and the policy remain. `mn -c`
completed, no `nfd`, `nlsr` or `mnexec` child remained, and no container or
iTiger command ran.

