# Tasks: NDNSF-Repo Design Slides

**Input**: Design documents from `specs/071-ndnsf-repo-design-slides/`

**Organization**: Tasks are grouped by independently reviewable audience outcomes.

## Phase 1: Setup

- [x] T001 Create `docs/NDNSF-REPO/slides/` and Spec Kit feature structure.
- [x] T002 Verify CodeGraph index and collect current Repo implementation evidence.
- [x] T003 Record accepted/rejected outline decisions in `specs/071-ndnsf-repo-design-slides/research.md`.

## Phase 2: Foundational Content

- [x] T004 Define the ordered frame contract in `specs/071-ndnsf-repo-design-slides/contracts/deck-content-contract.md`.
- [x] T005 Create the reusable Beamer theme, footer, callout, and TikZ styles in `docs/NDNSF-REPO/slides/main.tex`.

## Phase 3: Architecture Story (US1)

**Goal**: Explain the problem, boundary, layering, and deployment roles.

**Independent Test**: Frames 2-5 identify responsibility ownership without reading source code.

- [x] T006 [US1] Author problem and responsibility-boundary frames in `docs/NDNSF-REPO/slides/main.tex`.
- [x] T007 [US1] Author layered architecture and deployment-role frames in `docs/NDNSF-REPO/slides/main.tex`.

## Phase 4: Data and Control Mechanisms (US2)

**Goal**: Explain the object lifecycle from naming through repair.

**Independent Test**: A reader can narrate insert, fetch, placement, catalog, deletion, retention, and repair paths from the diagrams.

- [x] T008 [US2] Author namespace, manifest, and data-reference frames in `docs/NDNSF-REPO/slides/main.tex`.
- [x] T009 [US2] Author INSERT, payload-adapter, and fetch/verification frames in `docs/NDNSF-REPO/slides/main.tex`.
- [x] T010 [US2] Author capability/placement and object-level catalog frames in `docs/NDNSF-REPO/slides/main.tex`.
- [x] T011 [US2] Author catalog sync, tombstone, retention, and repair frames in `docs/NDNSF-REPO/slides/main.tex`.

## Phase 5: Integration and Evidence (US3)

**Goal**: Explain security boundaries, application reuse, current evidence, and limitations.

**Independent Test**: The closing frames distinguish verified functionality from future scale/performance work.

- [x] T012 [US3] Author security/trust and UAV/DI integration frames in `docs/NDNSF-REPO/slides/main.tex`.
- [x] T013 [US3] Author MiniNDN validation and design-takeaway frames in `docs/NDNSF-REPO/slides/main.tex`.
- [x] T014 [US3] Add reproducible build instructions in `docs/NDNSF-REPO/slides/README.md`.

## Phase 6: Build and Visual Acceptance

- [x] T015 Build `docs/NDNSF-REPO/slides/main.pdf` with two pdfLaTeX passes.
- [x] T016 Verify page count, extracted text, log warnings, and source requirements.
- [x] T017 Render all pages, inspect rendered page groups and dense pages, and fix clipping, overlap, or unreadable labels.
- [x] T018 Mark all completed tasks and run `git diff --check`.

## Dependencies & Execution Order

- T001-T004 establish scope and evidence.
- T005 blocks slide authoring because it defines the common visual system.
- T006-T013 are authored in deck order and share one file, so they execute sequentially.
- T014 can be written after the build commands are final.
- T015-T018 form the final acceptance gate.

## Implementation Strategy

Build the complete narrative once, compile early, then use rendered-page inspection to tune density and diagram geometry. Runtime code and unrelated documentation remain untouched.
