# Quickstart: Spec 137

This document describes the planned execution workflow. The commands become
valid only after their corresponding tasks in `tasks.md` are implemented and
verified.

## 1. Inspect The Active Feature

```bash
cat .specify/feature.json
.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks
```

Expected feature:

```text
specs/137-svs-serial-production-offload
```

## 2. Run The Pre-Implementation Gates

```bash
codegraph status .
python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py \
  specs/137-svs-serial-production-offload --strict
```

Do not implement or execute formal experiments while the audit verdict is
`BLOCK`.

## 3. Build Once

```bash
python3 Experiments/build_svs_serial_production_offload.py \
  --base 6bb34545b4f89f1f6c265a68c18f1a40ade413eb \
  --boost-root /usr/local/boost-1.71 \
  --output build/spec137-four-core
```

Review:

```bash
cat build/spec137-four-core/source-manifest.json
cat build/spec137-four-core/linkage.txt
sha256sum build/spec137-four-core/bin/svs-serial-production-offload
```

Both modes must reference this exact binary.

## 4. Create A New Campaign And Preflight

```bash
CAMPAIGN_DIR=results/spec137-svs-serial-production-offload/<new-campaign-id>

sudo -E python3 Experiments/NDN_SVS_Serial_Production_Offload_Minindn.py \
  --campaign "$CAMPAIGN_DIR" \
  --preflight
```

Preflight must close:

- no-op pacer +/-2%;
- common instrumentation overhead budget;
- runtime mode expansion;
- Face/worker thread ownership;
- `max_active_sync_signers=1`;
- zero production fallback;
- conservation and shutdown/drain;
- two-node/two-process routes and delivery.

## 5. Run The Non-Formal Pilot Once

```bash
sudo -E python3 Experiments/NDN_SVS_Serial_Production_Offload_Minindn.py \
  --campaign "$CAMPAIGN_DIR" \
  --pilot

cat "$CAMPAIGN_DIR/pilot/rate-selection.json"
```

The runner—not the operator—selects the formal rate. Pilot data cannot be cited
as a formal treatment estimate.

## 6. Seal Before Formal Execution

```bash
sudo -E python3 Experiments/NDN_SVS_Serial_Production_Offload_Minindn.py \
  --campaign "$CAMPAIGN_DIR" \
  --seal

cat "$CAMPAIGN_DIR/campaign-manifest.json"
```

Verify one binary hash, one frozen rate, and exactly six AB/BA/AB cells.

## 7. Run All Six Cells Once

```bash
sudo -E python3 Experiments/NDN_SVS_Serial_Production_Offload_Minindn.py \
  --campaign "$CAMPAIGN_DIR" \
  --run-formal
```

Do not rerun a failed or unfavorable ordinal. Inspect progress through receipts:

```bash
ls -1 "$CAMPAIGN_DIR/receipts"
cat "$CAMPAIGN_DIR/formal/campaign-cells.csv"
```

## 8. Verify And Analyze

```bash
python3 Experiments/analyze_svs_serial_production_offload.py \
  --campaign "$CAMPAIGN_DIR" \
  --verify \
  --report specs/137-svs-serial-production-offload/evidence/offload-proof-report.md
```

Required outputs:

```text
analysis/run-metrics.csv
analysis/paired-contrasts.csv
analysis/stage-breakdown.csv
analysis/traffic-breakdown.csv
analysis/conclusion.json
specs/137-svs-serial-production-offload/evidence/offload-proof-report.md
```

## 9. Close

Run the post-implementation Spec Kit audit, verify protected evidence hashes,
and mark tasks complete only from actual source/test/receipt evidence. Remove
the isolated worktree only after all patch, tree, linkage, binary, and campaign
hashes are preserved.
