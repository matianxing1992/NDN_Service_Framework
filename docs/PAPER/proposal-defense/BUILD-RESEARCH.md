# Research-first Proposal Sources

The September 2026 revision uses shared narrative sources:

- `main.tex` and `en/main.tex` load `en/chapters/research-revision.tex`.
- `main_ch.tex` and `ch/main.tex` load `ch/chapters/research-revision.tex`.
- Each language shares its existing `authorization-rationale.tex`.
- `protocol-overview.tex` is the common vector figure.
- `slides/main.tex` loads `slides/research-slides.tex`; `main_35min.tex` is a compatibility entry to the same consolidated talk, not a separately maintained argument.
- Earlier numbered chapter fragments are retained historical material and are not loaded by these entries.

Build English with `latexmk -norc -pdf -bibtex -interaction=nonstopmode -halt-on-error main.tex` from the appropriate directory. Build Chinese with `-xelatex` instead of `-pdf`. Build slides from `slides/` with `latexmk -norc -pdf -interaction=nonstopmode -halt-on-error main.tex`. Isolated output directories are recommended. Keep bibliography basenames unqualified; the alternate entry directories link to the shared additional bibliography.

`review_rendered.py` creates per-page images, contact sheets, bounds/word-count reports, and optional presenter-reference notes from PDF. It requires PyMuPDF and Pillow. These checks do not certify semantic truth. The maintained PPTX converter is `slides/generate_hybrid_editable_pptx.py`; use a new, nonexistent `/tmp/ndnsf-*/ndnsf-build` directory or its approved repository build root. Existing temporary directories require the converter's own marker. It preserves editable text and takes notes through `--notes-tex`.

Final PDF/PPTX/source hashes, build results and evidence boundaries are recorded in `research-revision-validation.json` and `research-revision-audit.md`. No experiment should be rerun merely to rebuild the documents. The 35-minute label is a retained filename; speaking time still requires rehearsal with the advisor's requested slot.

The latest DNMP-throughline revision uses `validate_dnmp_throughline.py` and the
`dnmp_throughline_revision` validation record. `validate_invocation_scope.py` and
`validate_sentence_review.py` retain earlier checks and raw-directory identities;
do not run them against newer PDFs and present historical evidence as a new check.
The root, `en/`, and `ch/` copies of `ref.bib` are independent files; synchronize
any edited bibliographic entry across all three before rebuilding.
