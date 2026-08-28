# Feature Specification: Submission-Ready NDNSF Evidence

**Feature Branch**: `spec170-wip`

**Created**: 2026-08-11

**Status**: Ready for implementation

**Input**: User description: "Revise the NDNSF paper, add or modify experiments only when necessary, preserve the core content of the May 20 manuscript, and make the innovation and advantages clear enough for submission."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Defensible Quantitative Claims (Priority: P1)

As an author, I can trace every quantitative statement, table, and figure in the manuscript to a retained experiment record or remove the unsupported precision before submission.

**Why this priority**: A reviewer must be able to distinguish measured evidence from historical recollection, inference, or illustrative mechanism behavior.

**Independent Test**: Review every quantitative manuscript item against one claim-to-evidence ledger; the story passes only when every item has a reproducible source or is explicitly removed or downgraded.

**Acceptance Scenarios**:

1. **Given** a numerical table in the manuscript, **When** its evidence entry is opened, **Then** the experiment conditions, independent repetitions, raw inputs, analysis result, and integrity identifier are available.
2. **Given** an inherited result whose original evidence cannot be recovered, **When** the manuscript is finalized, **Then** the exact number is absent unless a new matched experiment replaces it.
3. **Given** a negative or neutral control, **When** results are summarized, **Then** it is retained and the conclusion remains bounded rather than being hidden by a positive subset.

---

### User Story 2 - Preserve and Sharpen the Scientific Story (Priority: P2)

As an author, I can preserve the May 20 paper's essential service transaction, authorization, Provider selection, and evaluation content while making the actual novelty and measured advantages unmistakable.

**Why this priority**: The revision must strengthen the contribution without silently replacing the paper with a different system or overstating generic NDN, ABE, Sync, or RPC properties as new inventions.

**Independent Test**: Compare the old and new manuscripts section by section and confirm that every core mechanism remains represented while the title, abstract, contributions, related work, evaluation, and conclusion use the same bounded claim language.

**Acceptance Scenarios**:

1. **Given** the May 20 manuscript, **When** it is compared with the final manuscript, **Then** the four-object transaction, producer-scoped data, service authorization, multi-Provider selection, runtime API, and principal evaluation remain covered.
2. **Given** prior work on NDN services, ABE, and endpoint-oriented RPC, **When** novelty is stated, **Then** the paper attributes existing primitives and claims only their NDNSF-specific composition and behavior.
3. **Given** an experiment that shows an advantage only under switching opportunities or redundant coverage, **When** the abstract and conclusion are read, **Then** neither implies universal success-rate, latency, or reliability superiority.

---

### User Story 3 - Reviewer-Auditable Submission Package (Priority: P3)

As a reviewer or artifact evaluator, I can understand the experimental design and reproduce the reported summaries without relying on private conversation history, temporary files, or undocumented machine state.

**Why this priority**: A technically correct manuscript is still vulnerable if its evidence package does not expose provenance, comparability, and exclusions.

**Independent Test**: Starting only from the manuscript and its artifact index, a reader can locate the input, configuration, analysis, and result for every retained evaluation item.

**Acceptance Scenarios**:

1. **Given** the artifact index, **When** a table or figure identifier is selected, **Then** exactly one canonical evidence package and its analysis procedure are identified.
2. **Given** a comparison between NDNSF and a baseline, **When** the experimental manifest is examined, **Then** topology, workload, timing, deadline, availability trace, and measurement boundary are matched or every difference is disclosed.
3. **Given** the final manuscript, **When** it is built and inspected, **Then** it satisfies the ten-body-page constraint, has resolved citations and references, and contains no unreadable or clipped content.

### Edge Cases

- An old result is documented only in a commit message but its script and raw records are gone.
- A retained raw run exists but no reliable analysis can reconstruct the published value.
- Current behavior differs materially from an inherited table because the implementation or dependency changed.
- A rerun produces a neutral or negative result rather than the historical advantage.
- Multiple candidate runs exist for one claim but differ in topology, timeout, or measurement boundary.
- A table is reproducible but consumes space needed for a more central contribution.
- The target venue later imposes anonymity, artifact, disclosure, or reference-page rules not known during this feature.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The final manuscript MUST preserve the May 20 manuscript's core Request--ACK--Selection--Response transaction, producer-scoped Data model, service authorization, multi-Provider selection, application-facing runtime, and principal evaluation narrative.
- **FR-002**: Every quantitative claim, table, and figure MUST map to one canonical retained evidence package containing conditions, repetitions, raw or minimally processed inputs, analysis output, and integrity identifiers.
- **FR-003**: Any inherited quantitative item that cannot meet FR-002 MUST be replaced by a matched current experiment, rewritten without unsupported precision, or removed.
- **FR-004**: New comparison experiments MUST hold workload, topology, measurement window, deadlines, availability traces, and success definitions equal across systems except for the mechanism intentionally under test.
- **FR-005**: Performance results MUST use independent process repetitions and report variation or uncertainty rather than presenting one run as a population estimate.
- **FR-006**: Experiment registration MUST identify the primary outcome, controls, exclusions, and analysis rule before confirmatory results are inspected.
- **FR-007**: The evidence record MUST retain negative and neutral controls and MUST prohibit selecting only favorable seeds, rates, or mobility windows.
- **FR-008**: The final novelty statement MUST distinguish NDNSF's composition and service semantics from prior NDN Data naming, signatures, synchronization, ABE primitives, and endpoint-resolution mechanisms.
- **FR-009**: The paper MUST characterize advantages only under the conditions actually supported by retained evidence, including explicit limitations for unconditional mobility success and stable known endpoints.
- **FR-010**: Auxiliary mechanism evaluations MAY be shortened or removed when they are not reproducible or do not materially support a central contribution, provided the corresponding mechanism remains accurately described.
- **FR-011**: The final submission package MUST include a table/figure-to-artifact index that does not depend on temporary directories, private session state, or untracked historical recollection.
- **FR-012**: The final PDF MUST contain no more than ten body pages excluding references, resolve every citation/reference, and remain visually readable at normal review scale.
- **FR-013**: Venue-dependent anonymity, disclosure, artifact, and formatting checks MUST remain explicitly pending until a target venue is supplied; the manuscript MUST NOT claim venue compliance before then.

### Key Entities

- **Manuscript Claim**: A scientific or quantitative statement, with scope, location, and allowed wording.
- **Evidence Package**: The canonical conditions, inputs, repetitions, analysis output, integrity identifiers, and result supporting one or more claims.
- **Experiment Cell**: One system and condition combination under a fixed comparison design.
- **Matched Comparison**: A group of cells sharing all control variables except the intended mechanism.
- **Artifact Index Entry**: The link between a manuscript table or figure and its canonical evidence package.
- **Core Content Item**: A May 20 mechanism, API, security property, or evaluation concept that must remain represented in the revision.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 100% of numerical claims, tables, and figures in the final manuscript have a canonical artifact-index entry or are removed.
- **SC-002**: 100% of retained comparative performance cells have at least three independent process repetitions; central performance claims have at least five unless a documented power or precision analysis justifies fewer.
- **SC-003**: Every retained comparison reports a variation or uncertainty measure and includes all registered control cells.
- **SC-004**: A section-by-section comparison confirms that all six core content items in FR-001 remain represented in the final manuscript.
- **SC-005**: The title, abstract, contribution list, evaluation, discussion, and conclusion contain no contradiction about the scope of NDNSF's measured advantage.
- **SC-006**: A clean build produces ten body pages plus references with zero unresolved citations, unresolved references, fatal typesetting errors, clipped elements, or unreadable figures/tables.
- **SC-007**: An independent evidence audit can navigate from every retained table or figure to its conditions and summary without consulting chat history or temporary files.

## Assumptions

- The May 20 PDF is the preservation baseline; preservation means retaining its scientific core, not retaining every historical number, diagram, or auxiliary table.
- The current NDNSF runtime and its pinned dependencies are the authority for replacement experiments.
- Existing registered mobility and authorization evidence remains valid only within its documented scope and does not need a broader campaign unless its provenance check fails.
- Missing physical hardware does not block submission if the unverifiable physical-node table is removed or reduced to a non-quantitative implementation note.
- The final venue will be selected later; this feature produces a venue-neutral, reviewer-auditable manuscript and artifact package.
