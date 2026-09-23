# Research Decisions: NDNSF-Repo Design Slides

## Decision 1: Use the Existing NDNSF Technical Deck Style

**Decision**: Reuse the white background, Memphis blue titles, restrained callouts, and compact footer from `docs/NDNSFDI/slides/main.tex`.

**Rationale**: This keeps project presentations visually consistent and already works with the installed pdfLaTeX toolchain.

**Alternatives considered**: A dark theme would be visually distinct but inconsistent with the proposal and DI decks; a branded template would add dependencies without improving the technical story.

## Decision 2: Make TikZ the Primary Visual Medium

**Decision**: Draw architecture, data path, catalog, and repair flows with TikZ; use tables only where comparison is the actual message.

**Rationale**: The current repository has no canonical Repo figure assets. TikZ keeps diagrams versioned, legible, and aligned with the LaTeX source.

**Alternatives considered**: Screenshots of source or README text would be harder to read and would not explain relationships. Generated bitmap diagrams would be less maintainable.

## Decision 3: Separate C++ Core Facts from Python Control-Plane Extensions

**Decision**: Slides identify `RepoCore`, `RepoNode`, `RepoClient`, basic catalog structures, and placement as C++ implementation surfaces. Query filters, retention classes, repair planning, sidecar catalog synchronization, and auto-repair are presented as the current Python/deployment control path built on the same repo service.

**Rationale**: The C++ manifest currently contains the compact object fields, while richer lifecycle metadata and repair orchestration live in the Python integration layer. Combining them without qualification would misstate implementation ownership.

**Alternatives considered**: Presenting a single undifferentiated architecture is simpler but technically inaccurate.

## Decision 4: Describe the Repo as Opaque Storage

**Decision**: State explicitly that the repo stores opaque application payloads or signed Data wire packets. The application owns encryption policy and verifies returned object size/hash against the manifest.

**Rationale**: This matches the current README and `RepoStoreBackend` contract. It also prevents the deck from claiming that storage nodes decrypt or re-authorize application content.

**Alternatives considered**: Calling the repo a trusted content validator would overstate current behavior and blur the application/repo boundary.

## Decision 5: Treat Existing MiniNDN Markers as Functional Evidence

**Decision**: Summarize the validated scenarios and show the canonical command and success marker families. Do not invent throughput, scale, or durability numbers.

**Rationale**: The generic MiniNDN regression covers catalog gossip, policy, tombstones, queries, UAV data products, repair, and auto-repair, but it is not a production-scale benchmark.

**Alternatives considered**: Rerunning the full regression is unnecessary for a documentation-only change; quoting unmeasured performance would be misleading.
