# T035 Tiger Candidate 179470

## Identity

```text
jobId: 179470
submissionId: spec162-submission-t009-smoke-706b10a3-009
sourceManifestSha256:
706b10a3626f2bfef5bd097ae5941a79592c58afc83fbb112ad55405cd41f6c3
sifSha256:
c56dd8d9451941719c5ae60d09fd9c92e1e90325dbb0457263d2aa050edae024
stageManifestSha256:
cd9bb9c37dd2b7780cf76a2b3080d2b58fa27a4e16b22e5b6f377ee70e50e787
nodes: itiger07, itiger08, itiger09
```

## Terminal result

Slurm recorded:

```text
179470|CANCELLED by 64102|0:0|00:02:39|itiger[07-09]
179470.batch|CANCELLED|0:0|00:02:39|itiger07
179470.extern|CANCELLED|0:0|00:02:39|itiger[07-09]
179470.0|CANCELLED|0:0|00:01:59|itiger[07-09]
```

The operator cancelled the candidate before repository registration after the
local full Spec 162 suite found that the newly added artifact backend
configuration omitted the mandatory explicit operator state root. All three
NFDs and all six inter-node faces/routes had already completed successfully.

No bootstrap token or selection key existed in the partial directory. The
default Slurm cancellation terminated the batch shell before its EXIT trap
wrote `result.json`; this negative evidence is retained rather than repaired
in place. The replacement candidate adds an explicit state root and a
graceful cancellation protocol (`USR1` to the batch shell) that writes
`state=CANCELLED`, removes private material, and exits before Slurm teardown.
