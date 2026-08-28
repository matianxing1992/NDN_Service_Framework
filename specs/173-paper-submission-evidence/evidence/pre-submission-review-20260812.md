# NDNSF Pre-Submission Editorial Review

Date: 2026-08-12  
Mode: Academic Research Suite `academic-paper-reviewer`, quick editorial review  
Manuscript: `docs/PAPER/named-data-network-service-framework-paper/NDNSF.pdf`  
PDF SHA-256: `8a49385572d9ead2f7bd5d3e8a41ab861b9b047ef0033b3ffe6c056fc8c8e0d4`

## Field and Maturity

| Dimension | Assessment |
|---|---|
| Primary field | Named Data Networking / information-centric networking systems |
| Secondary fields | Distributed services, service authorization, mobile/edge systems |
| Research paradigm | Systems design with controlled experimental evaluation |
| Methodology | Protocol/runtime design, correctness regressions, matched emulation, physical-node integration |
| Maturity | Pre-submission draft |
| Review stance | ACM ICN-style systems reviewer; venue-specific compliance is not assessed because no target venue is fixed |

## Editorial Recommendation

**Minor revision before external circulation.** The paper now has a coherent and defensible core: it claims a specific four-object service transaction, transaction-integrated ABE-backed authorization, and request-scoped runtime Provider selection rather than claiming NDN naming, Sync, ABE, or endpoint-independent resolution as new. Its positive performance statements are conditional and are accompanied by neutral controls. The remaining issues are mostly precision and presentation problems, but two of them could cause a reviewer to misread the comparison as broader than the evidence supports.

## Strengths

1. **Bounded novelty.** The title, contribution list, Related Work, Discussion, and Conclusion consistently present the contribution as an NDNSF-specific composition rather than a new cryptographic or NDN primitive.
2. **Negative evidence is retained when it answers a live claim.** The paper reports the one-Provider latency disadvantage, the absence of an unconditional mobility advantage, and the failed high-load comparison. The historical loss points and ACK-window-confounded custom-selection performance table are removed rather than promoted as inferential evidence.
3. **Claim-to-evidence traceability.** Every live table and figure is covered by the Spec 173 artifact index, and the automated manuscript audit reports no unsupported numerical precision.
4. **Baseline limitations are disclosed.** The manuscript acknowledges resolver-supplied gRPC addresses, optional health checking and retry, NSC shared names and queue recovery, and the fact that `NSC-SEQ-4` is only the evaluated sequential harness.
5. **Implementation support exists.** Current source contains the generic service API, token-gated Selection, and bounded response-level reselection over stored valid ACK candidates; the prose does not claim Request republication.

## Required Revisions

### R1 — Use exact baseline identities in the abstract

**Severity:** Major wording issue; small edit.

The abstract says “versus sequential gRPC” and “versus NSC,” although the evidence is specifically for `gRPC-SEQ-4` and the evaluated `NSC-SEQ-4` four-prefix harness. Elsewhere the manuscript correctly explains that the harness is not NSC's full shared-name/queue architecture. The abstract is the most likely passage to be read independently and should use the exact labels.

The same sentence should say **successful-response p95 latency**. Failed requests do not have an observed response latency, and the paper does not claim success superiority.

### R2 — Avoid equivalence language without an equivalence design

**Severity:** Minor wording issue.

The abstract says NDNSF “matches parallel gRPC completion.” The experiment reports a paired mean difference and confidence interval, but it does not register an equivalence margin or run an equivalence test. “Achieves comparable observed completion” is supported and matches the Introduction, Evaluation, and Conclusion.

### R3 — Make the deterministic transition table self-interpreting

**Severity:** Major presentation issue.

Table `tab:provider_transition_discovery` compresses two baseline configurations into entries such as `0 / 357`. A reader can mistake this for a fraction rather than the results for static A--C and pre-registered A--D. The `Endpoints` heading also makes NDNSF's `0` look configuration-free even though the caller supplies a service name and the experiment relies on NDN routing and Sync state.

Split A--C and A--D into separate rows, or label the paired columns explicitly. Prefer “caller-supplied targets” over “endpoints,” and keep the accompanying statement that the test isolates the evaluated service-namespace path rather than proving that gRPC or NSC lacks dynamic discovery mechanisms.

### R4 — State the conditioning of the mobility estimand once in the abstract

**Severity:** Minor scientific-precision issue.

The Evaluation and figure caption correctly identify the estimand as seed-level p95 over successful responses in the registered `SWITCH_REQUIRED` population, and they separately report completion. The abstract currently omits “successful-response.” Adding that phrase prevents a reviewer from interpreting the p95 difference as a failure-inclusive deadline metric.

## Residual Questions for External Reviewers

1. Is the four-object transaction plus transaction-integrated authorization sufficiently distinct from existing named-computation and service-call designs for the intended venue, or should the paper foreground explicit execution authority more strongly?
2. Is the registered switching population persuasive as a conditional mechanism evaluation, given that four individual seeds favor gRPC and failed requests are reported separately rather than included in the latency estimand?
3. Is `gRPC-PAR-4` accepted as an intentionally eager first-success control for Provider-work cost, rather than as a claim about normal gRPC deployment practice?
4. Does the venue expect an authorization-disabled ablation, concurrent-User scaling, or Controller/Attribute-Authority failure evaluation for a security-integrated systems contribution?

## Reproducibility and Submission Packaging

The current paper directory is ignored by `.gitignore` (`docs/*`) and its TeX, figures, and PDF are not tracked in the repository. Therefore commit `0d4a1340641f76be1e8eebd8d3c2a43ba58f498c` is only the repository base revision; it does not reconstruct the reviewed manuscript. The recorded per-file and PDF hashes protect integrity, but final submission still requires either:

- a dedicated tracked submission branch/archive containing the complete TeX source, bibliography, figures, generator, and PDF; or
- a content-addressed release archive with a manifest that lists every included file and SHA-256.

This packaging issue does not invalidate the scientific results, but it blocks a claim that the reviewed PDF is reproducible from the recorded Git revision alone.

## Recommended Revision Order

1. Correct the abstract labels, successful-response qualifier, and equivalence wording.
2. Clarify the transition table without changing its data or claim.
3. Rebuild and verify the ten-body-page limit.
4. Obtain the planned external technical read.
5. Freeze the accepted revision in a tracked or content-addressed submission bundle.

## Revision Disposition

The bounded revision was applied after the read-only review:

- R1 addressed: the abstract now names `gRPC-SEQ-4` and the evaluated `NSC-SEQ-4` harness.
- R2 addressed: “matches” is replaced by “achieves comparable observed completion.”
- R3 addressed: the transition table now gives A--C and A--D separate rows and labels caller targets explicitly.
- R4 addressed: the abstract now identifies the metric as successful-response p95 latency.

Rebuilt manuscript SHA-256: `8a49385572d9ead2f7bd5d3e8a41ab861b9b047ef0033b3ffe6c056fc8c8e0d4`. The submission-packaging issue remains open.
