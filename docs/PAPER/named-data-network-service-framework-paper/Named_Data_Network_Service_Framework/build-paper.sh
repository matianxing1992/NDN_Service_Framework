#!/usr/bin/env bash
# Build the clean manuscript and a text-marked comparison to NDNSF_Base.
set -euo pipefail

paper_source_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
paper_output_dir="$(dirname -- "$paper_source_dir")"
paper_baseline_dir="$paper_output_dir/NDNSF_Base"
paper_clean_build="$paper_output_dir/.overleaf-build"
paper_diff_build="$paper_output_dir/.comparison-build"

for tool in latexmk pdflatex bibtex latexdiff pdfinfo python3; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
done
for source in "$paper_source_dir/NDNSF.tex" "$paper_baseline_dir/NDNSF.tex" "$paper_source_dir/diff-preamble.tex"; do
    test -f "$source" || { echo "Missing source: $source" >&2; exit 1; }
done

mkdir -p -- "$paper_clean_build" "$paper_diff_build"
cd -- "$paper_source_dir"
latexmk -pdf -interaction=nonstopmode -halt-on-error \
    -outdir="$paper_clean_build" NDNSF.tex
python3 "$paper_source_dir/make-comparison.py" \
    "$paper_baseline_dir/NDNSF.tex" "$paper_source_dir/NDNSF.tex" \
    "$paper_source_dir/diff-preamble.tex" "$paper_diff_build/NDNSF_comparison.tex"
latexmk -pdf -interaction=nonstopmode -halt-on-error \
    -outdir="$paper_diff_build" "$paper_diff_build/NDNSF_comparison.tex"

for log in "$paper_clean_build/NDNSF.log" "$paper_diff_build/NDNSF_comparison.log"; do
    if grep -Eq 'There were undefined (references|citations)|LaTeX Warning: (Citation|Reference).*undefined' "$log"; then
        echo "Unresolved citations/references; outputs not published. Check $log" >&2
        exit 1
    fi
done
pdfinfo "$paper_clean_build/NDNSF.pdf" >/dev/null
pdfinfo "$paper_diff_build/NDNSF_comparison.pdf" >/dev/null
cp -- "$paper_clean_build/NDNSF.pdf" "$paper_output_dir/NDNSF.pdf"
cp -- "$paper_diff_build/NDNSF_comparison.pdf" "$paper_output_dir/NDNSF_comparison.pdf"
echo "Clean: $paper_output_dir/NDNSF.pdf"
echo "Comparison: $paper_output_dir/NDNSF_comparison.pdf"
