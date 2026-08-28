# Quickstart: Rebuild the Submission Evidence

The campaign wrapper and analyzer named below are deliverables of this Spec.
Until they exist, use this document as the acceptance contract, not as a claim
that the campaign has already run.

## 1. Preflight

From the repository root, verify the build, dependency linkage, MiniNDN, and
registration without starting a measured run:

```bash
./waf configure
./waf build

python3 Experiments/paper_submission_campaign.py \
  --registration specs/173-paper-submission-evidence/contracts/experiment-registration.yaml \
  --preflight

sudo -n python3 Experiments/paper_submission_campaign.py \
  --registration specs/173-paper-submission-evidence/contracts/experiment-registration.yaml \
  --pilot --output-root results/spec173-paper-submission-pilot
```

Pilot output is diagnostic only and must not enter the manuscript.

## 2. Confirmatory campaign

```bash
sudo -n python3 Experiments/paper_submission_campaign.py \
  --registration specs/173-paper-submission-evidence/contracts/experiment-registration.yaml \
  --confirmatory --resume \
  --output-root results/spec173-paper-submission-confirmatory
```

The wrapper must execute the frozen matrix, capture exact commands and
revisions, rotate system order deterministically, and stop rather than silently
changing a failed cell. `--resume` may skip only a complete valid cell whose
registration, toolchain, command, summary, and artifact hashes all match; it
must never reuse a partial or invalid run.

## 3. Freeze compact evidence

```bash
python3 scripts/analyze_paper_submission_evidence.py \
  --registration specs/173-paper-submission-evidence/contracts/experiment-registration.yaml \
  --input results/spec173-paper-submission-confirmatory \
  --output specs/173-paper-submission-evidence/evidence
```

Expected durable outputs include per-run manifests, normalized run summaries,
across-repetition statistics, hashes, exclusions, and `artifact-index.json`.
Raw process logs remain under `results/`.

## 4. Manuscript and audit

```bash
cd docs/PAPER/named-data-network-service-framework-paper
latexmk -pdf -interaction=nonstopmode NDNSF.tex
cd -

python3 scripts/analyze_paper_submission_evidence.py \
  --audit-manuscript docs/PAPER/named-data-network-service-framework-paper/NDNSF.tex \
  --artifact-index specs/173-paper-submission-evidence/evidence/artifact-index.json
```

Acceptance requires ten body pages excluding references, no unresolved
citations or fatal LaTeX errors, and no quantitative manuscript item lacking a
supported artifact-index entry.
