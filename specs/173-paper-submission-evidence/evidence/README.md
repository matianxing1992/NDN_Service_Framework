# NDNSF Paper Evidence Map

This directory is the reviewer-facing entry point for the NDNSF manuscript. `artifact-index.json` maps every live table and figure label to a source or evidence package and retains explicit dispositions for removed historical precision.

## Reproduce the Spec 173 evidence

Run from the repository root with the pinned toolchain described by `toolchain-manifest.json`:

```bash
sudo -n python3 Experiments/paper_submission_campaign.py \
  --registration specs/173-paper-submission-evidence/contracts/experiment-registration.yaml \
  --toolchain-manifest specs/173-paper-submission-evidence/evidence/toolchain-manifest.json \
  --confirmatory \
  --output-root results/spec173-paper-submission-confirmatory-v2

sudo -n python3 scripts/analyze_paper_submission_evidence.py \
  --registration specs/173-paper-submission-evidence/contracts/experiment-registration.yaml \
  --input results/spec173-paper-submission-confirmatory-v2 \
  --output results/spec173-paper-submission-confirmatory-v2/analysis
```

The runner uses deterministic block shuffling, immutable attempt directories, matched-block failure handling, and hash-verified resume. It never retries based on observed outcomes. `confirmatory-v2.md` records the frozen hashes and admitted, qualified, excluded, and removed claims.

## Rebuild and verify the paper

```bash
cd docs/PAPER/named-data-network-service-framework-paper
latexmk -pdf -interaction=nonstopmode -halt-on-error NDNSF.tex
pdfinfo NDNSF.pdf
pdffonts NDNSF.pdf
pdftoppm -png -r 110 NDNSF.pdf /tmp/ndnsf-paper-page
```

The requested format is ten body pages plus References on page 11. `SUBMISSION_AUDIT.md` records the final hash, log checks, font checks, and visual inspection.

## Evidence groups

- **Architecture/design tables and figures:** current manuscript design/implementation contracts and source paths in `artifact-index.json`; these are explanatory, not measured outcomes.
- **Authorization:** Spec 172 claim/evidence matrix and retained publication/onboarding artifacts.
- **Mobility and Provider work:** Spec 171 frozen holdout, transition, and six-seed work-efficiency packages.
- **NSC baseline boundary:** `nsc-baseline-audit.md` distinguishes the measured NSC-SEQ-4 harness from NSC's shared-name/task-queue architecture and registers the requirements for any optional faithful control.
- **Pre-submission editorial review:** `pre-submission-review-20260812.md` records the remaining wording, baseline-presentation, and submission-packaging issues.
- **One-Provider and selection controls:** Spec 173 frozen registration, three process repetitions, normalized runs, aggregates, exclusions, and correctness regression.
- **Dual-certificate runtime validation:** `dual-cert-single-provider-20260812.json` records a separate 60-second, one-Provider MiniNDN validation of RSA encryption/ECDSA signing, epoch-level key wrapping/cache reuse, the 800-byte SVS safety bound, and the explicit admission-disabled setting. It is diagnostic evidence only and is not a multi-Provider comparison.
- **Table V NDNSF replacement:** `runtime/table5-matched-fixed-20260813/manifest.json` records three independent current-build repetitions for both NDNSF normal and targeted modes on the same wired `memphis`/`ucla`/`arizona` topology and 10-RPS workload as the frozen gRPC/NSC rows. Normal is `177.75 +/- 13.08 ms` mean and `192.69 +/- 31.54 ms` p95; targeted is `89.86 +/- 0.79 ms` mean and `91.95 +/- 1.59 ms` p95 with fixed 256-pair refill sizing. One matched trace records three 256-pair stores, zero refill failures, zero fallback requests, and fixed next-batch/pool-depth 256. The earlier default-adaptive rows are superseded because small demand-derived batches inflated latency. The `28.22 +/- 0.65 ms` default-topology run and frozen `403.93 +/- 0.39 ms` row remain diagnostic only.
- **Current multi-Provider pilot:** `dual-cert-three-provider-open5-60s-20260812.json` records a current-runtime three-Provider, 5-RPS, 60-second NDNSF pilot with balanced A/B/C selection. It is correctness/selection evidence only; it is not a paired baseline or mobility result.
- **Loss recovery:** exact retained historical summaries and hashes in `historical/recovered-evidence.json`; qualified because exact revision and independent repetitions were not recoverable.

## Required interpretation boundaries

- The one-Provider table is contextual: gRPC has the lowest latency; NDNSF performs additional Sync, authorization, ACK, and Selection work.
- The registered 100-RPS NDNSF cells fail the offered-load validity threshold; no incomplete three-system comparison is made.
- Admission control restricts injection under overload but does not add Provider capacity and is not a contribution.
- Selective ACK is correctness evidence only.
- The registered custom queue run remains archived as a negative diagnostic, but it is removed from the live manuscript because the one-shot 100 ms ACK window confounds policy behavior with ACK-path timing. The manuscript retains correctness evidence only.
- Mobility benefits are conditional on Provider-switch opportunities; all-request controls show no universal success or latency advantage.
- The physical-node result is qualitative integration evidence because the historical baseline Provider sets were unequal.

## Frozen identifiers

- Registration SHA-256: `64fce70880fd0451999d5920b9c7730a9c1b74578ad0a193f356529dcdff5310`
- Toolchain-manifest SHA-256: `a633411bacce596e20ceed585d16a36966a929394d97fa8682da865ee3f1ed7c`
- Campaign-manifest SHA-256: `d30892cc1c18e3883d63c0a18b69e5c19833b35b20794f7cc18d4e5c8e73f535`

Do not rewrite the registration or toolchain manifest after inspecting results. A changed protocol, workload, registration, or binary starts a new campaign root.
