# Quickstart: Stream Latency Diagnosis and Optimization

## 1. Focused deterministic gates

Run the Stream unit test that crosses at least three Mapping blocks, the UAV latest-join/FEC test, and the Python correlation fixture. The initial regression must fail before the fix and pass afterward.

## 2. Corrected 60-second baseline

Use `Experiments/NDNSF_UAV_GUI_Minindn.py` with zero loss, file camera mode, mapped prefetch, WARN NFD logs, a unique `results/spec121-continuity-baseline-*` directory, and a 60-second measured window. Preserve the exact command and environment in `run-summary.json`.

Accept only when:

- provider and consumer both progress across at least three Mapping blocks;
- source delivery occurs in the final ten seconds;
- future-hit ratio is at least 99% for eligible items;
- no callback exception or unexplained ACTIVE stall occurs;
- startup and steady metrics use valid correlation identities.

## 3. Candidate probes

Evaluate one variable per unique identity. Candidate order:

1. latest-join reorder initialization;
2. FFmpeg cold-start/lifecycle behavior;
3. FEC grouping only if steady reorder evidence points there;
4. adaptive window only if future-hit or PIT evidence points there.

Do not rerun a failed identity. Preserve negative results.

## 4. Superseded matched evidence

Do not start additional Spec 121 performance cells. The frozen one-pair evidence lacks exact camera-source-to-decoded-frame identity and is diagnostic only. Spec 122 T009 creates fresh, counterbalanced baseline/candidate and trace-off/on identities after its source-frame oracle and runtime correlation gates pass.

## 5. Stop conditions

Stop the matrix if continuity fails, security/protocol tests fail, correlation validity falls below 95%, a candidate changes more than one variable, or results show no repeatable benefit.
