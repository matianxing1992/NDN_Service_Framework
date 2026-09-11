#!/usr/bin/env python3
"""Focused checks for the 2026-09-11 document revision (not product tests)."""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import zipfile
import fitz

base = Path(__file__).resolve().parent
root = base.parents[2]
raw = root / '.codex-tmp/proposal-sentence-review-20260911'
pdf_names = ['main.pdf', 'main_ch.pdf', 'en/main.pdf', 'ch/main.pdf',
             'slides/main.pdf', 'slides/main_35min.pdf',
             'slides/speaker_notes.pdf', 'slides/speaker_notes_35min.pdf']
docs = {n: fitz.open(base/n) for n in pdf_names}
texts = {n: [p.get_text() for p in d] for n,d in docs.items()}
checks = {}
for a,b in [('main.pdf','en/main.pdf'), ('main_ch.pdf','ch/main.pdf'),
            ('slides/main.pdf','slides/main_35min.pdf'),
            ('slides/speaker_notes.pdf','slides/speaker_notes_35min.pdf')]:
    assert texts[a] == texts[b], (a,b)
checks['paired_entry_text_equality'] = True
logs = [p for p in raw.glob('build-*/*.log') if not p.name.startswith('driver')]
assert len(logs) == 8, [str(p) for p in logs]
for p in logs:
    assert not re.search(r'Overfull|Missing character|undefined|^!', p.read_text(errors='replace'), re.M), str(p)
checks['eight_final_latex_logs_no_overfull_missing_glyph_undefined'] = True
checks['underfull_warnings'] = sum(p.read_text(errors='replace').count('Underfull') for p in logs)
render = json.loads((raw/'final-render/render-audit.json').read_text())
lo = json.loads((raw/'lo-render/render-audit.json').read_text())
for item in render+lo:
    assert not any(p['text_outside_page'] for p in item['pages']), item['file']
assert len(render[0]['pages']) == len(lo[0]['pages']) == 40
assert all(p['words'] <= 100 for p in render[0]['pages'] if p['page'] < 38)
checks['ordinary_slide_max_words'] = max(p['words'] for p in render[0]['pages'] if p['page'] < 38)
checks['all_pdf_and_lo_text_bounds'] = True
checks['reference_density_exemptions'] = [38,39,40]
before = fitz.open(raw/'before/slides.pdf')
after = docs['slides/main.pdf']
def numbers(page):
    t=page.get_text(clip=fitz.Rect(0,0,page.rect.width,page.rect.height*.94))
    return re.findall(r'\d+(?:[,.]\d+)*',t)
for p in [30,31,36]:
    assert numbers(before[p-1]) == numbers(after[p-1]), p
checks['unchanged_work_di_transition_numbers'] = [30,31,36]
for v in ['599', '598', '539', '600', '10%']:
    assert v in texts['slides/main.pdf'][36], v
checks['historical_loss_counts_unchanged'] = True
for v in ['54.57', '53.80', '55.47', '97.97', '98.33', '100.00']:
    assert v in texts['slides/main.pdf'][27]
holdout = json.loads((root/'specs/171-four-provider-mobility-advantage/evidence/opportunity-holdout-results-20260809/holdout-summary.json').read_text())
success = {s:sum(r[s]['success'] for r in holdout['per_seed']) for s in ['ndnsf','grpc','nsc']}
assert success == {'ndnsf':1311,'grpc':1297,'nsc':1296}
for v in ['1,311','1,297','1,296','597','992','200','2618','2916','2315']:
    assert v in texts['slides/main.pdf'][28], v
checks['holdout_success_counts_from_existing_record'] = success
checks['holdout_seed_boundary'] = holdout['seeds']
conv = json.loads((raw/'editable-text-manifest.json').read_text())['validation']
assert conv['source_span_count'] == conv['assigned_once_count']
assert conv['background_extractable_text_characters'] == 0
with zipfile.ZipFile(base/'slides/NDNSF_proposal_hybrid_editable.pptx') as z:
    slides = [n for n in z.namelist() if re.fullmatch(r'ppt/slides/slide\d+.xml',n)]
    notes = [n for n in z.namelist() if re.fullmatch(r'ppt/notesSlides/notesSlide\d+.xml',n)]
    assert len(slides) == len(notes) == 40
checks['pptx_and_notes_count'] = 40
checks['editable_text'] = conv
checks['notes_parser'] = '2/2 PASS (separate unittest invocation)'
ledger = json.loads((base/'sentence-claim-ledger-20260911.json').read_text())
for n,h in ledger['source_sha256'].items():
    assert hashlib.sha256((base/n).read_bytes()).hexdigest() == h, n
checks['claim_inventory_current'] = {'units':len(ledger['units']),**ledger['counts']}
checks['manual_review_scope'] = 'All listed prose sources; all PDF/LO contact sheets; enlarged changed argument/table/figure pages. This is not a formal semantic certificate.'
revision = {'date':'2026-09-11', 'scope':'Sentence-level bilingual proposal and 40-slide revision',
            'verdict':'DOCUMENT_CHECKS_PASS_RESEARCH_GAPS_EXPLICIT',
            'checks':checks, 'pages':{n:len(d) for n,d in docs.items()},
            'visual_review':'Final PDF and LibreOffice contact sheets plus enlarged affected pages; fixed figure inherited double-spacing overlap.',
            'client_limit':'LibreOffice round-trip inspected; Microsoft PowerPoint and Google Slides not run.',
            'product_validation':'NOT_RUN; no experiment or product changes in this unit.',
            'raw_evidence_root':str(raw.relative_to(root)),
            'review':'sentence-review-20260911.md'}
path = base/'research-revision-validation.json'
report = json.loads(path.read_text())
report.setdefault('historical_checks',{}).setdefault('before_sentence_review_20260911',
    {k:report.get(k) for k in ['scope','checks','pdfs','pptx','visual_review']})
report['scope']=revision['scope']; report['verdict']=revision['verdict']
report['checks']=checks; report['sentence_review_revision']=revision
report['pdfs']={n:{'pages':len(d),'text_outside_page':[]} for n,d in docs.items()}
build_counts = re.search(r'Created (\d+) textboxes with (\d+) runs, (\d+) native triangle bullets',
                        (raw/'pptx-build-final-r2.log').read_text())
assert build_counts
report['pptx']=dict(conv,editable_textboxes=int(build_counts[1]),
                   native_triangle_bullets=int(build_counts[3]),notes_pages=40)
report['visual_review']=revision['visual_review']
report['raw_evidence_root']=revision['raw_evidence_root']
report['observed_repository_head']=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
report['implementation_source_check_scope']='Read-only Core challenge/policy/revocation inspection and Spec184 T007 evidence; no product test rerun or completion promotion.'
paths=set(report['files'])|{'sentence-review-20260911.md','sentence-claim-ledger-20260911.json','review_sentence_claims.py','validate_sentence_review.py'}
report['files']={n:hashlib.sha256((base/n).read_bytes()).hexdigest() for n in sorted(paths)}
path.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
(raw/'final-checks.json').write_text(json.dumps(revision,ensure_ascii=False,indent=2)+'\n')
print(json.dumps({'pages':revision['pages'],'checks':checks},ensure_ascii=False))
