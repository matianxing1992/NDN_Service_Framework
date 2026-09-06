# Quickstart: Spec 143

## Safety preflight

```bash
test -f results/spec142-svs-ndnsf-runtime-profile/campaign-20260724T012559Z/qualification-verdict.json
git status --short
python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py \
  specs/143-svs-zero-loss-fetch-causality --strict
```

Record the Spec 142 tree/hash inventory before and after Spec 143. Do not run a
Spec 142 command.

## Focused tests and build

```bash
python3 -m unittest tests.python.test_spec143_svs_zero_loss_fetch_causality
python3 Experiments/build_svs_zero_loss_fetch_causality.py
```

The build script must preserve exact source/library hashes and verify the
runtime linkage before any MiniNDN process starts.

## Exactly-once diagnostic

```bash
sudo -E python3 Experiments/NDN_SVS_Zero_Loss_Fetch_Causality_Minindn.py \
  --stage worker-400
```

Expected runtime is approximately 85 seconds plus setup/teardown. Do not invoke
the command a second time to replace any outcome.

## Analyze

```bash
python3 Experiments/analyze_svs_zero_loss_fetch_causality.py \
  results/spec143-svs-zero-loss-fetch-causality/<campaign-id>
```

The report is successful only when the raw hashes validate and at least 95% of
observed measurement-window timeouts are classified. Zero timeouts is
`INCONCLUSIVE`.

