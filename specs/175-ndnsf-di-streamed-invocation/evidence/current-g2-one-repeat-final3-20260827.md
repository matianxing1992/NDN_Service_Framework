# Current-source G2 one-repeat rerun after timing-parser tests — 2026-08-27

The final timing-parser mutation tests changed only the test/source subject, so
the I01--I20 matrix was rerun once against the resulting source seal. All 20
registered cases executed and passed.

```text
sourceSeal=/tmp/spec175-source-seal-final3-zosCrm.json
sourceSealSha256=sha256:60bb8b3bdd81ac2fe0f9e28fdb487510cf91f0cd13025105c2a5f4799399c08c
seed=1750001
healthyRepeats=1
cases=I01-I20
resultCount=20
missingCases=[]
status=PASS
manifest=/tmp/spec175-g2-final3-PnnkBL/manifest.json
manifestSha256=sha256:817a65855b641a2c06166a58baf01aefe842b18429eec7824217cf2779ea2a01
```

This remains a one-repeat current-source checkpoint. It does not close the
three-repeat T020/T022 qualification, exact-SIF replay, CUDA, or Tiger gates.
