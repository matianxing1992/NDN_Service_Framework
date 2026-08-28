# Spec 168 Job Surface

`spec168_campaign.py` is the single admission/campaign identity entrypoint. It
intentionally contains no copied Spec 162 launcher and no model/SIF payload.

Authoritative reusable remote identities are in
`../evidence/baseline-manifest.json`. Campaign V3 binds the bounded local
tiny-Qwen fixture separately as `localFixtureManifestDigest`; the baseline's
Qwen3-0.6B and larger negative control are bound as
`remoteSmallStageManifestDigest` and `remoteLargeStageManifestDigest`. A job
wrapper may resolve existing Spec 162 and Spec 167 files only after verifying
their recorded digest. It must create a new immutable Spec 168 source/campaign
identity for any changed wrapper, analyzer, or runtime code.

Campaign V3 also binds `sourceBundleDigest`, `localGateManifestDigest`, and
`exactSifPreflightDigest`. It represents local exact-overlay validation and the
bounded remote exact-SIF/CUDA preflight as separate ordered phases; neither is
silently substituted for the other.

After Gates A and B pass, one bounded single-node `sbatch` is admitted solely
for Gate C when the 8 GiB development host lacks Apptainer or CUDA. It must use
the exact frozen SIF, remote small-model identity, and immutable output path and
cannot support a distributed-inference claim. No three-node campaign `sbatch`
is admitted until Gates A-D pass for the same source/SIF/model identities.

Commands:

```bash
python3 spec168_campaign.py derive \
  --baseline ../evidence/baseline-manifest.json \
  --bindings <candidate-bindings.json>

python3 spec168_campaign.py freeze \
  --baseline ../evidence/baseline-manifest.json \
  --bindings <candidate-bindings.json> \
  --output-root <immutable-campaign-manifest-root>

python3 spec168_campaign.py validate \
  --manifest <campaign-root>/campaign-manifest.json

python3 spec168_campaign.py claim \
  --manifest <campaign-root>/campaign-manifest.json \
  --result-root <unique-result-root>
```

`freeze` atomically writes only experiment, schedule, and campaign JSON.
`claim` atomically creates only `run-claim.json`. Duplicate identities,
manifest mutation, missing bindings, baseline/runtime/stage mismatch, and a
second writer fail closed. The tool never submits work; every remote phase in
the generated schedule has `manualRemoteSubmissionOnly=true`.
