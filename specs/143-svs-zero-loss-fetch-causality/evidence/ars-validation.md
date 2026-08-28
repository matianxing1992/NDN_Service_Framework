# ARS Experiment Validation

## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: validate
- Verification Status: MEASURED
- Version Label: code_result_v1

## Internal Validity

- The binary, NDN-SVS library, source hashes, runtime profile, commands,
  topology, timing, and CPU affinity were frozen before the network cell.
- Both peers reached the target attempted rate exactly and exercised the same
  bidirectional V3/RSA path.
- Full Interest name, nonce, and consumer attempt identifier prevent retry
  attempts from being merged.
- Durations are computed only inside one process clock domain. No cross-peer
  monotonic-clock subtraction is used.
- Spec 142 remained unchanged and the conditional inline cell remained closed.

## Threats And Fallacy Scan

| Risk | Treatment |
|---|---|
| One cell cannot establish variance | No confidence interval, significance, or repeatability claim is made |
| Zero configured loss is not zero failure | Report says only “0% configured loss”; it does not infer physical loss |
| No producer callback is not proof of packet loss | Class is named `NO_PRODUCER_OBSERVATION`; NFD/PIT/transport causes remain unresolved |
| Producer `Face::put` is not proof of consumer delivery | It is reported as a boundary, not successful transport |
| Same name with another nonce is not proof of PIT aggregation | It is explicitly a secondary observation and hypothesis |
| TRACE perturbs the system | Trace bytes, process CPU, RSS, and thread count are disclosed; performance claims are prohibited |
| Mapping prefix overlap can create a false store miss | Analyzer precedence was corrected and the post-cell revision is content-addressed |
| Post-cell analyzer change could hide a rerun | Raw trace hashes and the `networkCellRerun=false` revision receipt preserve provenance |

## Reproducibility And Evidence Quality

The causal classification is reproducible from the retained peer traces and
corrected analyzer. Coverage is 829/829. The experiment is adequate to reject
producer store unavailability as the dominant explanation and to locate the
remaining boundary around NFD/transport delivery. It is not adequate to choose
a production recovery policy.

## Verdict

**PASS for the diagnosis-only Spec 143 claim.**

The next experiment must use deterministic sampling and NFD-level PIT/face
counters to separate Interest aggregation, forwarding omission, queue loss,
and Data return failure without repeating the 237 MB TRACE overhead.
