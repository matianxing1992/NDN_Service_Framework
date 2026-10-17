---
name: review
description: Review code or manuscript changes against a pinned baseline for project fit, necessity, correctness, evidence and clarity. Use for branch, PR, worktree or paper revision reviews; revise findings only when the user authorizes edits.
---

# Review

Review every changed unit along two independent axes:

- **Standards and context:** ownership, project conventions, compatibility,
  security, lifecycle and, for manuscripts, terminology and readability.
- **Requirements and necessity:** requested behavior or research question,
  supporting evidence, omissions, scope creep and the smallest sufficient change.

An improvement is justified only when it serves a current requirement or fixes a
demonstrated defect. More code, more detail or more documentation is not itself
an improvement. Preserve approved behavior, security checks, failure handling,
necessary observability and evidence limits.

## Pin scope and comparison

Use the user's baseline. For a manuscript, an already designated original or
comparison baseline is sufficient; for a worktree review, include staged,
unstaged and relevant untracked changes. Ask only when available context cannot
resolve an ambiguity that changes the review.

- Branch/PR: resolve the comparison ref and record its merge-base with HEAD.
  Review the merge-base-to-HEAD diff.
- Local work: review the baseline-to-worktree diff and relevant new files;
  do not use a committed-only diff that silently drops pending edits.
- Manuscript: compare maintained sources with the fixed original, including
  changed or removed paragraphs, tables, figures, captions and bibliography.
  Record source/PDF identities. A generated comparison PDF is a view of the
  changes, not the editable authority.

State the included paths and any excluded or missing evidence. Preserve the
baseline and unrelated work. Do not create an issue-tracker setup or new review
documentation merely because optional context is absent.

## Establish context from evidence

Read applicable AGENTS.md and current user requirements first. Identify the
affected caller, owner, inputs, state transitions and failure paths in source
before judging a code change. Use CodeGraph first only where an index exists.
Read relevant specs, issue descriptions, contracts and measurement records when
available. Design documents describe intent; they do not prove implementation
or measured behavior. Do not turn every optional document into binding policy.

For a paper, establish the research questions, intended contribution, application
logic, protocol vocabulary, experimental configurations and evidence limits.
Check whether a new claim describes an existing mechanism, an implementation
change, a measurement or an inference. Keep those categories distinguishable.

## Review each changed unit

Account for every hunk or coherent manuscript change, including deletions.
Group changes serving the same outcome; use a compact inventory inside the
existing review rather than a separate report process.

For each unit, establish:

1. **Need:** the current requirement or concrete defect it addresses, and the
   observable behavior or reader understanding it changes.
2. **Fit:** consistency with the project's actual ownership, application logic,
   interfaces, terminology and approved scope.
3. **Support:** source or data supporting it, relevant negative cases and
   compatibility. Mark missing or indirect evidence as unverified.
4. **Minimum:** whether an existing path, smaller patch or shorter explanation
   would satisfy the same requirement with less state and maintenance burden.

Classify the outcome as keep, simplify, remove/revert, correct or unverified.
Do not invent a defect merely to recommend fewer lines. A necessary explanation
of a security boundary or negative result must remain even when it adds length.

### Code checks

Apply affected repository engineering rules to the actual path: naming and wire
parity, trust and authorization, ownership, dependency admission, bounded async
lifecycle, cleanup and build/install registration. Reject parallel state
machines, speculative abstractions, hidden fallback behavior and duplicate
conversions when no current requirement justifies them. Static review is not a
runtime qualification. Do not launch builds or tests without task authorization.

### Manuscript checks

Every addition must help explain the research question, mechanism, method,
result or necessary limitation. Prefer one clear explanation at the right
location; remove repeated caveats, redundant figures and unnecessary execution
details while preserving reproducibility and evidence boundaries.

Information needed to conduct the review is not automatically suitable for the
manuscript. For each explanation, identify what the intended reader needs it
for. Keep it in the paper only if it explains the contribution, supports a
result, enables reproduction or prevents a material misinterpretation.

- Condense debugging chronology, internal counters, acceptance receipts and
  reviewer-facing reminders into the relevant method, result or limitation.
  Keep operational details in existing evidence or scripts unless the reader
  needs them; do not create new documentation merely to relocate deleted prose.
- State scientific definitions directly: overlapping CPU measurements, for
  example, rather than instructions telling the reader not to add columns.
  Preserve parameters that affect fairness or interpretation; do not remove
  essential methods simply because they also appear in implementation logs.
- Include a derived model or threshold only when it answers the research
  question under defensible assumptions. When measured results already answer
  it, prefer those results over an unnecessary extrapolation and its caveats.

- Use concrete actors and actions. Define unfamiliar terms once. Resolve unclear
  pronouns, acronym overload, long clauses and ambiguous quantities.
- Check names, units, denominators, phases, cache conditions, baseline capability,
  uncertainty and scope. For example, an initial-issuance benefit must not be
  worded as a benefit for every permission update.
- Distinguish object counts, encoded bytes, link traffic, elapsed latency and CPU.
  Separate supported capability, enabled configuration and measured result.
- For each material citation-dependent claim, check the cited primary work's
  relevant passage, figure or experiment. Match the claim's actor, mechanism,
  conditions and scope to that source; a citation marker or another paper's
  paraphrase is insufficient. Distinguish a proposed design from what the
  cited implementation or evaluation actually demonstrated. If the source
  cannot be checked, mark support unverified and narrow or remove the claim.
- Trace material in-project results to the original run records and analysis,
  with the measured version, workload and security/retry policy. Repeated
  manuscript numbers, task status and derived PDFs are not independent evidence.
  Treat missing provenance as a gap, not proof that a result is false. Attribute
  a gain to one mechanism only when the comparison controls other material
  differences; otherwise describe the combined effect and its limits.
- Keep claim, table/figure, caption, abstract and conclusion consistent. Shorten
  repeated numbers and methodological details that do not change interpretation.
- Retain failures, inconclusive comparisons and applicable limits. Do not make
  statements stronger to make the contribution appear more novel.
- Each figure/table needs a specific comparison or question. Axes, legends and
  captions must be readable and interpretable without color alone. Prefer a
  compact existing display when it conveys the same necessary information.
- Inspect source changes and rendered affected pages. Text diff markup does not
  audit changed figure binaries, bibliography content or layout.

After shortening, recheck statistic definitions, time origins, initialization
amortization and negative outcomes against evidence. Concision must not change
the measured quantity or hide a failure. Reassess earlier keep decisions when
the user's current focus changes; a prior review verdict is not evidence of
publication relevance.

"Clear and unambiguous" is an acceptance criterion to examine, not a guarantee
that static reading proves every possible interpretation.

## Independent review and correction

Use two read-only reviewers in parallel when delegation is available: one for
Standards and context, one for Requirements and necessity. Provide both with the
same pinned snapshot, relevant requirements, evidence paths and full scope.
Both inspect necessity; manuscript reviewers also inspect clarity. Keep their
initial judgments independent. If delegation is unavailable, report that limit
and perform the two passes separately.

Report actionable findings by axis, with file/line or figure/page, the relevant
requirement, consequence and smallest sufficient correction. Distinguish
confirmed defects, judgment calls and missing evidence. Summarize coverage and
unresolved limits; a finding count alone does not prove completeness.

For review-only requests, leave files unchanged. When correction is authorized,
fix supported findings in the maintained source, then re-review the affected
snapshot. For manuscripts, rebuild required clean/comparison deliverables and
inspect changed pages using the project's maintained workflow. Do not edit
generated PDFs directly or silently revise a fixed baseline. Preserve any
required verification gates and record actual checks, not intended checks.
