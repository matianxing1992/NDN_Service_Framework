#!/usr/bin/env python3
"""Check the invocation/collaboration narrative revision, not protocol behavior."""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import zipfile

import fitz

base = Path(__file__).resolve().parent
root = base.parents[2]
raw = root / '.codex-tmp/proposal-invocation-scope-20260911'
names = ['main.pdf', 'en/main.pdf', 'main_ch.pdf', 'ch/main.pdf',
         'slides/main.pdf', 'slides/main_35min.pdf',
         'slides/speaker_notes.pdf', 'slides/speaker_notes_35min.pdf']
docs = {n: fitz.open(base/n) for n in names}
texts = {n: [p.get_text() for p in d] for n, d in docs.items()}
checks = {}
for a, b in zip(names[::2], names[1::2]):
    assert texts[a] == texts[b], (a, b)
checks['paired_entry_text_equality'] = True
for n in ['main.pdf', 'main_ch.pdf']:
    assert 'Keywords:' in texts[n][1], n
    assert 'Table of Contents' in texts[n][2], n
checks['abstract_and_keywords_fit_one_page'] = True
en = (base/'en/chapters/research-revision.tex').read_text()
assert en.index('The Initial NDNSF Foundation') < en.index('For example, nearby unmanned')
assert 'The initial NDNSF prototype selects one Provider from multiple candidates' in en
assert 'The table describes the single-Provider invocation.' in en
assert 'not by the initial four-message flow' in en
slides = (base/'slides/research-slides.tex').read_text()
assert 'Start with a Task' not in slides
assert 'The Task Creates Three Requirements' not in slides
assert 'RQ2 Extension: From Positive ACKs to a Role Plan' in slides
checks['scoped_narrative_guards'] = True
logs = [p for p in raw.glob('build-*/*.log') if not p.name.startswith('driver')]
assert len(logs) == 8, logs
for p in logs:
    assert not re.search(r'Overfull|Missing character|undefined|^!', p.read_text(errors='replace'), re.M), p
checks['eight_final_latex_logs_no_overfull_missing_glyph_undefined'] = True
checks['underfull_warnings'] = sum(p.read_text(errors='replace').count('Underfull') for p in logs)
render = json.loads((raw/'final-render/render-audit.json').read_text())
lo = json.loads((raw/'lo-render/render-audit.json').read_text())
for item in render+lo:
    assert not any(p['text_outside_page'] for p in item['pages']), item['file']
assert len(render[0]['pages']) == len(lo[0]['pages']) == 40
assert all(p['words'] <= 100 for p in render[0]['pages'] if p['page'] < 38)
checks['all_pdf_and_lo_text_bounds'] = True
checks['ordinary_slide_max_words'] = max(p['words'] for p in render[0]['pages'] if p['page'] < 38)
checks['reference_density_exemptions'] = [38, 39, 40]
before = fitz.open(raw/'before/slides.pdf')
def numbers(page):
    text = page.get_text(clip=fitz.Rect(0, 0, page.rect.width, page.rect.height*.94))
    return re.findall(r'\d+(?:[,.]\d+)*', text)
pages = [28, 29, 30, 31, 36, 37]
for p in pages:
    assert numbers(before[p-1]) == numbers(docs['slides/main.pdf'][p-1]), p
checks['unchanged_experiment_numbers_on_slides'] = pages
conv = json.loads((raw/'editable-text-manifest.json').read_text())['validation']
assert conv['source_span_count'] == conv['assigned_once_count']
assert conv['background_extractable_text_characters'] == 0
with zipfile.ZipFile(base/'slides/NDNSF_proposal_hybrid_editable.pptx') as z:
    assert len([n for n in z.namelist() if re.fullmatch(r'ppt/slides/slide\d+.xml', n)]) == 40
    assert len([n for n in z.namelist() if re.fullmatch(r'ppt/notesSlides/notesSlide\d+.xml', n)]) == 40
checks['pptx_and_notes_count'] = 40
checks['editable_text'] = conv
ledger = json.loads((base/'sentence-claim-ledger-20260911.json').read_text())
for n, h in ledger['source_sha256'].items():
    assert hashlib.sha256((base/n).read_bytes()).hexdigest() == h, n
checks['claim_inventory_current'] = {'units': len(ledger['units']), **ledger['counts']}
revision = {
    'date': '2026-09-11', 'scope': 'Invocation foundation before collaboration extension; bilingual proposal, slides and notes',
    'verdict': 'DOCUMENT_CHECKS_PASS_RESEARCH_GAPS_EXPLICIT', 'checks': checks,
    'pages': {n: len(d) for n, d in docs.items()},
    'visual_review': 'PDF and LibreOffice contact sheets; enlarged opening and changed protocol pages.',
    'client_limit': 'LibreOffice round-trip; Microsoft PowerPoint and Google Slides not run.',
    'product_validation': 'NOT_RUN; no product changes or new experiments.',
    'raw_evidence_root': str(raw.relative_to(root)),
    'review': 'invocation-scope-review-20260911.md',
}
path = base/'research-revision-validation.json'
report = json.loads(path.read_text())
report.setdefault('historical_checks', {}).setdefault('before_invocation_scope_20260911',
    {k: report.get(k) for k in ['scope', 'checks', 'pdfs', 'pptx', 'visual_review']})
report['invocation_scope_revision'] = revision
for k in ['scope', 'verdict', 'checks', 'visual_review', 'raw_evidence_root']:
    report[k] = revision[k]
report['pdfs'] = {n: {'pages': len(d), 'text_outside_page': []} for n, d in docs.items()}
stats = re.search(r'Created (\d+) textboxes with (\d+) runs, (\d+) native triangle bullets',
                  (raw/'pptx-build.log').read_text())
assert stats
report['pptx'] = dict(conv, editable_textboxes=int(stats[1]), native_triangle_bullets=int(stats[3]), notes_pages=40)
report['observed_repository_head'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
report['implementation_source_check_scope'] = 'Narrative-only revision; initial single-Provider scope supplied by author and maintained records; current CodeGraph lookup is not historical snapshot proof.'
paths = set(report['files']) | {'invocation-scope-review-20260911.md', 'validate_invocation_scope.py'}
report['files'] = {n: hashlib.sha256((base/n).read_bytes()).hexdigest() for n in sorted(paths)}
path.write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')
(raw/'final-checks.json').write_text(json.dumps(revision, ensure_ascii=False, indent=2)+'\n')
print(json.dumps({'pages': revision['pages'], 'checks': checks}, ensure_ascii=False))
