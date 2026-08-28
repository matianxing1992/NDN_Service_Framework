# Core Content Comparison

This checklist records the final manuscript disposition of the May 20 scientific core. Precision was removed where the historical artifact package was not recoverable; the underlying supported concept was retained.

| Core item | Final manuscript location | Disposition |
|---|---|---|
| Data-centric transaction and unified service naming | Introduction; Design, `Service Naming and Transactions` | Preserved and foregrounded. |
| Dynamic multi-Provider discovery | Introduction; Design; Evaluation, `Matched Mobility and Native Provider Discovery` | Preserved with bounded, matched-baseline claims. |
| ACK-metadata selection | Design, `Provider Discovery and Selection`; Evaluation, `Runtime Selection Controls` | Preserved. Selective ACK and custom selection remain correctness evidence; the ACK-window-confounded policy-performance table is removed. |
| ABE-backed service-semantic authorization | Introduction; Design, `Authorization Model`; Evaluation, authorization evidence | Preserved and foregrounded without claiming ABE itself as new. |
| UserToken/ProviderToken replay protection | Design, authorization invariants; Evaluation, authorization matrix | Preserved. |
| Producer-namespace response naming | Design, naming table; Discussion | Preserved with verification and forwarding rationale. |
| Admission control | Design, `Provider Discovery and Selection`; Evaluation, `Runtime Selection Controls` | Preserved only as an optional resource-protection mechanism. Exact table removed; not a contribution. |
| Loss recovery | Evaluation, `Single-Provider Baseline and Loss Recovery` | Preserved as qualified historical canonical evidence. |
| Physical deployment feasibility | Evaluation, `Physical-Node Integration` | Preserved qualitatively. Exact unequal-Provider comparison removed. |
| Four-Provider mobility | Evaluation, mobility subsections | Preserved with the registered holdout and explicit baseline assumptions. |
| Work efficiency versus endpoint retry | Evaluation, `Provider-Work Efficiency` | Preserved with execution counts and baseline boundaries. |
| Streaming and retrieval APIs | Implementation | Preserved concisely as implementation facilities, not unmeasured performance claims. |
| Limitations and scope | Discussion and Limitations; evaluation qualifications | Preserved and strengthened with the offered-load exclusion, success-conditioned mobility estimand, and explicit baseline boundaries. |

## Precision changes

- Replaced the unsupported four-rate one-Provider table with the registered 10-RPS, three-repetition result.
- Removed exact admission-control, Selective-ACK population, and physical-node percentages.
- Removed the custom-selection performance table after audit showed that the fixed ACK window, rather than Provider service delay, dominated completion; retained only the registered decision-point correctness evidence.
- Retained the 100-RPS cells in the artifact package but excluded NDNSF by the pre-registered offered-load rule and made no incomplete cross-system claim.
