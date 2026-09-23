# Research-first Proposal Sources

Latest scope checkpoint (2026-09-17): see `minimal-revision-checklist-20260917.md`.
The seven-chapter structure keeps UAV and DI separate and consolidates evaluation
and the timeline in Chapter 6. `validate_minimal_revision.py --run <before-and-build-directory>`
checks this round against its archived pre-edit sources; durable results are in
`minimal-revision-validation-20260917.json`. Earlier validation records are historical.

The September 2026 revision uses shared narrative sources:

- `main.tex` and `en/main.tex` load `en/chapters/research-revision.tex`.
- `main_ch.tex` and `ch/main.tex` load `ch/chapters/research-revision.tex`.
- Each language shares its existing `authorization-rationale.tex`.
- `protocol-overview.tex` is the common vector figure.
- `slides/main.tex` loads `slides/research-slides.tex`; `main_35min.tex` is a compatibility entry to the same consolidated talk, not a separately maintained argument.
- Earlier numbered chapter fragments are retained historical material and are not loaded by these entries.

## File layout and entry-point policy

The repository intentionally keeps two entry points for each language so that
the root-level deliverables used in review and the language-specific build
directories can be checked independently. They are not four independent
proposal versions:

| Path | Role | Editing rule |
|---|---|---|
| `main.tex`, `main_ch.tex` | Root English/Chinese compatibility entries and shareable-document entry points | Keep synchronized with `en/main.tex` and `ch/main.tex`; do not treat them as separate manuscripts |
| `en/chapters/`, `ch/chapters/` | Canonical English/Chinese chapter sources | Edit the language chapter here |
| `en/main.tex`, `ch/main.tex` | Language-specific mirror build entries | Keep the entry metadata and abstract synchronized with the root entries |
| `main.pdf`, `main_ch.pdf` | Root-level shareable English/Chinese PDFs | Current deliverables referenced by review documents |
| `en/main.pdf`, `ch/main.pdf` | Language-directory mirror PDFs | Validation mirrors; not additional revisions |
| `build/`, `en/build/`, `ch/build/` | LaTeX auxiliary files and isolated build logs | Do not use as shareable manuscript versions |

The root and language-directory PDFs must be rebuilt together after source
changes. A stale mirror is an error, even when the page count appears to
match. Comparison PDFs and files under `revision-comparison/` are historical
review artifacts and are not current proposal sources.

Build English with `latexmk -norc -pdf -bibtex -interaction=nonstopmode -halt-on-error main.tex` from the appropriate directory. Build Chinese with `-xelatex` instead of `-pdf`. Build slides from `slides/` with `latexmk -norc -pdf -interaction=nonstopmode -halt-on-error main.tex`. Isolated output directories are recommended. Keep bibliography basenames unqualified; the alternate entry directories link to the shared additional bibliography.

`review_rendered.py` creates per-page images, contact sheets, bounds/word-count reports, and optional presenter-reference notes from PDF. It requires PyMuPDF and Pillow. These checks do not certify semantic truth. The maintained PPTX converter is `slides/generate_hybrid_editable_pptx.py`; use a new, nonexistent `/tmp/ndnsf-*/ndnsf-build` directory or its approved repository build root. Existing temporary directories require the converter's own marker. It preserves editable text and takes notes through `--notes-tex`.

Final PDF/PPTX/source hashes, build results and evidence boundaries are recorded in `research-revision-validation.json` and `research-revision-audit.md`. No experiment should be rerun merely to rebuild the documents. The 35-minute label is a retained filename; speaking time still requires rehearsal with the advisor's requested slot.

The latest DNMP-throughline revision uses `validate_dnmp_throughline.py` and the
`dnmp_throughline_revision` validation record. `validate_invocation_scope.py` and
`validate_sentence_review.py` retain earlier checks and raw-directory identities;
do not run them against newer PDFs and present historical evidence as a new check.
The root, `en/`, and `ch/` copies of `ref.bib` are independent files; synchronize
any edited bibliographic entry across all three before rebuilding.
