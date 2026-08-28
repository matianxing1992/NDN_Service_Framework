# Job 182515: v87 three-node control-plane canary

Job 182515 used campaign `spec168-campaign-v3-d23d84fafe684f52a379` on
`itiger07`-`itiger09`. Slurm recorded `FAILED (1:0)` after 4 minutes 44 seconds,
but the three-rank step completed successfully in 3 minutes 34 seconds. The
immutable remote output is retained under `raw/` and remains labelled `FAIL`.

The protocol execution itself completed:

- all three ranks mapped native core `sha256:50c059...` and extension
  `sha256:f0e072...`, including three fresh compatibility-child ABI checks;
- the user emitted three `NDNSF_SELECTION_PROVIDER_PROJECTION` records for one
  runtime request ID;
- all three providers recorded `selection received` and
  `collaboration handler running` for that request;
- the user received a schema-valid three-stage response with lineage
  `prompt -> Stage/0 -> Stage/1 -> Stage/2` in 1071.05 ms;
- no Qwen fetch, load, or inference marker occurred (`modelWorkCount=0`).

The batch failed only because the canary analyzer required the synthetic marker
`LLM_PIPELINE_USER_OK`, which the real user does not emit. The local unit fixture
had invented that marker and therefore could not detect the contract drift.
The corrected analyzer parses and validates the actual
`LLM_PIPELINE_USER_RESPONSE` JSON. `reanalysis.json` is the corrected analyzer's
PASS result over the untouched `raw/` evidence; it does not rewrite Slurm's or
the original analyzer's terminal state.

Lesson: gate fixtures must be seeded from retained real runtime records, and a
closure analyzer must validate semantic response fields rather than a marker
that is absent from the executable. A completed remote protocol run must not be
resubmitted merely to repair post-run evidence classification.
