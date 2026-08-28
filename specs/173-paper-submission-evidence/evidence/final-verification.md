# Spec 173 Final Verification

Date: 2026-08-12

## Verdict

PASS for the requested ten-body-page manuscript and reviewer-auditable evidence package. Venue-specific policy remains pending because no target venue is fixed.

## Frozen campaign and analysis

- Confirmatory command: `sudo -n python3 Experiments/paper_submission_campaign.py --registration specs/173-paper-submission-evidence/contracts/experiment-registration.yaml --toolchain-manifest specs/173-paper-submission-evidence/evidence/toolchain-manifest.json --confirmatory --output-root results/spec173-paper-submission-confirmatory-v2`
- Campaign result: 16/16 blocks and 37/37 cells reached verified terminal state; status `pass`; zero outcome-based retries.
- Selective-ACK correctness regression: pass.
- Analyzer result: pass; packet pseudoreplication disabled; process repetition is the independent unit; no significance claims at registered `n=3`.
- Exclusions: six admission-disabled overload cells and three 100-RPS NDNSF one-Provider cells failed the registered 80% offered-load rule. All are retained in `exclusions.json`.
- Registration SHA-256: `64fce70880fd0451999d5920b9c7730a9c1b74578ad0a193f356529dcdff5310`
- Toolchain-manifest SHA-256: `a633411bacce596e20ceed585d16a36966a929394d97fa8682da865ee3f1ed7c`
- Campaign-manifest SHA-256: `d30892cc1c18e3883d63c0a18b69e5c19833b35b20794f7cc18d4e5c8e73f535`
- Analysis-manifest SHA-256: `cadd897c69c13a203b8adeee9d2db90cb6a716d2c16679ef11e14889c53e8da9`
- Aggregate SHA-256: `cc6c3ffe76b83fb995b1e3333491ada49573bc6b085754f2267e14a2ee63c19b`
- Exclusions SHA-256: `ad113e1ccf6b6a7c339e69e91235b59a4268bea3fd5671d24d0698aa7da3bdce`
- Normalized-runs SHA-256: `656c8793e68cc4bd6388651197db49ecd8212aea4fc414014caa4006fb372bcf`

## Automated tests

- `python3 -m pytest -q tests/python/test_spec173_paper_submission_campaign.py tests/python/test_spec173_paper_submission_evidence.py tests/python/test_spec173_open_loop_window.py`: 18 passed.
- `./build/unit-tests --log_level=test_suite`: 464 test cases; no errors detected.
- `./build/integration-tests --log_level=test_suite`: 11 test cases; no errors detected.
- The full C++ suite includes streaming/reordering/FEC, authorization/onboarding, token/replay, generic dynamic API, multi-Provider selection, and DI fixture coverage.
- The MiniNDN confirmatory correctness cell supplies the final network-path Selective-ACK/custom-selection regression; host/default NFD was not substituted for final evidence.

## Manuscript and artifact audit

- `latexmk -pdf -interaction=nonstopmode -halt-on-error NDNSF.tex`: pass.
- PDF: ten body pages plus References on page 11; Letter paper; PDF 1.5.
- LaTeX log: no fatal error, undefined reference/citation, or overfull box.
- Fonts: all embedded subset Type 1 fonts.
- Rendering: all 11 pages rendered to PNG; contact sheet plus original-resolution page 5 and pages 8--9 inspected; no clipping, overlap, broken float, or unreadable table.
- PDF SHA-256: `8a49385572d9ead2f7bd5d3e8a41ab861b9b047ef0033b3ffe6c056fc8c8e0d4`.
- `qpdf` is not installed; `pdfinfo`, `pdffonts`, `pdftotext`, `pdfseparate`, and `pdftoppm` all parsed the PDF successfully.
- Artifact index schema/manuscript audit: pass.
- Table/figure bidirectional map: 13/13 live labels have artifact-index entries; no unindexed live label or stale supported/qualified entry.
- `git diff --check` over the Spec 173/paper change scope: pass.

## Claim consistency

- Admission control remains a short optional resource-protection mechanism. Its exact table is removed, it is not a contribution, and the paper states that it does not add service capacity.
- The new one-Provider table reports only the valid 10-RPS comparison and shows NDNSF's higher full-transaction latency rather than retaining the old unsupported advantage.
- The historical loss paragraph is removed because its archive lacks an exact Git revision and independent repetitions and does not establish a central comparative claim.
- The custom queue performance table is removed because its fixed 100-ms ACK window confounds policy behavior with ACK-path timing; only the registered correctness invariant remains.
- Selective ACK is correctness evidence only; physical-node deployment is qualitative integration evidence only.
- Optional NDNSD discovery is explicitly separated from request-time Provider willingness, and the response-recovery description is bounded to reselection from validated ACKs rather than Request republication.
- Central positive claims remain bounded to the four-object service transaction, transaction-integrated ABE-backed authorization, request-scoped runtime multi-Provider selection, conditional switching latency, and Provider-work efficiency under redundant coverage.

## Residual limitations and next action

- Target-venue anonymity, reference-page treatment, artifact, AI-disclosure, and template rules are unverified.
- The strongest next action is an external technical read, followed by a clean pinned submission checkout and archived source/PDF/evidence bundle. Do not launch another broad performance matrix for this manuscript.
