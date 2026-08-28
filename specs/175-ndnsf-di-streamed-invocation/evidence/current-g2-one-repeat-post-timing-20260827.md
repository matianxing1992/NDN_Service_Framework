# Current-source G2 one-repeat rerun after timing correction — 2026-08-27

The registered native I01--I20 matrix was rerun once after the Spec175
provider-timing/reporting correction. It used the newly generated content-bound
source seal and the existing integration binary; no case was skipped.

```text
sourceSeal=/tmp/spec175-source-seal-final-2Gmio0.json
sourceSealSha256=sha256:2573a675c65247583caba9aad716561238d807de07266a219ed8e6fc82771ff2
seed=1750001
healthyRepeats=1
cases=I01-I20
resultCount=20
missingCases=[]
status=PASS
manifest=/tmp/spec175-g2-post-timing-9Q3klV/manifest.json
manifestSha256=sha256:82515f2259b32d17209d7edcb6686bea975fc22fa24aa2e47a0114217806ca1e
```

This is a one-repeat current-source G2 checkpoint. T020 still requires the
final implementation queue to close and three fresh healthy repetitions before
the host/SIF promotion sequence can start.
