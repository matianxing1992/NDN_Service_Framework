#!/usr/bin/env python3
"""Check proposal-positioning artifacts; not a scientific or committee verdict."""
import argparse
import hashlib
import json
import re
import subprocess
import zipfile
from pathlib import Path

import fitz

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[2]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--raw', type=Path, required=True)
ap.add_argument('--base-ref', default='d0164b1c')
args = ap.parse_args()
raw = args.raw.resolve()

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def old(path):
    return subprocess.check_output(['git','show',args.base_ref+':'+str(path.relative_to(ROOT))],text=True,cwd=ROOT)

def frames(text):
    return re.findall(r'\\begin\{frame\}\{([^\n]+)\}\n(.*?)\\end\{frame\}',text,re.S)

modules = ['application-motivation','ndn-background','framework-architecture','invocation-data-services',
           'uav-workflows','di-workflows','evaluation-methods','authorization-rationale']
source_paths = []
for lang in ['en','ch']:
    path = BASE/lang/'chapters/research-revision.tex'
    before, after = old(path), path.read_text()
    assert len(re.findall(r'\\chapter\{',before)) == len(re.findall(r'\\chapter\{',after)) == 7
    assert re.findall(r'\\input\{[^}]+\}',before) == re.findall(r'\\input\{[^}]+\}',after)
    assert re.search(r'\\begin\{enumerate\}.*?\\end\{enumerate\}',before,re.S).group() == re.search(r'\\begin\{enumerate\}.*?\\end\{enumerate\}',after,re.S).group()
    source_paths.append(path)
    for name in modules:
        module = BASE/lang/'chapters'/(name+'.tex')
        assert module.read_text() == old(module), module
        source_paths.append(module)
    tables = lambda t: re.findall(r'\\begin\{table\}.*?\\end\{table\}',t,re.S)
    # Only the last table (the timeline wording) may differ.
    assert tables(before)[:-1] == tables(after)[:-1]

source = BASE/'slides/research-slides.tex'
before, after = frames(old(source)), frames(source.read_text())
assert len(before) == len(after) == 48  # excludes the titlepage frame
experiments = []
for i, ((title, body), (new_title, new_body)) in enumerate(zip(before,after), 2):
    if title.startswith(('Mobility:', 'Fewer Execution Starts', 'A Useful Negative Result','Backup:')):
        assert body == new_body, title
        experiments.append({'page':i,'before':title,'after':new_title,'body_byte_equal':True})
assert len(experiments) == 6
source_paths += [source, BASE/'main.tex',BASE/'main_ch.tex',BASE/'en/main.tex',BASE/'ch/main.tex']

names = ['main.pdf','en/main.pdf','main_ch.pdf','ch/main.pdf','slides/main.pdf',
         'slides/main_35min.pdf','slides/speaker_notes.pdf','slides/speaker_notes_35min.pdf']
docs = {n:fitz.open(BASE/n) for n in names}
for a,b in zip(names[::2],names[1::2]):
    assert [p.get_text() for p in docs[a]] == [p.get_text() for p in docs[b]], (a,b)
for n in ['main.pdf','main_ch.pdf']:
    assert 'Keywords:' in docs[n][1].get_text() and 'Table of Contents' in docs[n][2].get_text()
for n, doc in docs.items():
    for i,page in enumerate(doc):
        for block in page.get_text('dict')['blocks']:
            for line in block.get('lines',[]):
                for span in line['spans']:
                    x0,y0,x1,y1=span['bbox']
                    if span['text'].strip():
                        assert x0>=-1 and y0>=-1 and x1<=page.rect.width+1 and y1<=page.rect.height+1,(n,i+1,span['text'])
slide_pages=[]
for i,p in enumerate(docs['slides/main.pdf']):
    text=p.get_text(clip=fitz.Rect(0,0,p.rect.width,p.rect.height*.94))
    lines=[s.strip() for s in text.splitlines() if s.strip()]
    if not lines[0].startswith('References:'):
        assert len(text.split())<=100,(i+1,len(text.split()))
    slide_pages.append({'page':i+1,'title':lines[0],'words':len(text.split())})
logs=[raw/'latex-r2'/tag/(name+'.log') for tag,name in [
    ('root-en','main'),('root-ch','main_ch'),('en','main'),('ch','main'),
    ('slides','main'),('slides35','main_35min'),('speaker_notes','speaker_notes'),('speaker_notes_35min','speaker_notes_35min')]]
for log in logs:
    assert not re.search(r'Overfull|Missing character|undefined|^!',log.read_text(errors='replace'),re.M),log
pptx=BASE/'slides/NDNSF_proposal_hybrid_editable.pptx'
with zipfile.ZipFile(pptx) as z:
    for prefix in ['ppt/slides/slide','ppt/notesSlides/notesSlide']:
        assert sum(bool(re.fullmatch(prefix+r'\d+\.xml',n)) for n in z.namelist())==49
manifest=json.loads((BASE/'slides/build/proposal-structure-20260914/editable-text-manifest.json').read_text())
editable=manifest['validation']
assert editable['source_span_count']==editable['assigned_once_count']
assert editable['background_extractable_text_characters']==0
assert len(fitz.open(raw/'lo/NDNSF_proposal_hybrid_editable.pdf'))==49
lo=json.loads((raw/'lo-render/render-audit.json').read_text())[0]
assert not any(p['text_outside_page'] for p in lo['pages'])
report={
    'date':'2026-09-14','verdict':'PROPOSAL_STRUCTURE_CHECKS_PASS_RESEARCH_REMAINS',
    'base_ref':args.base_ref,'scope':'Organization, status wording, preservation and rendering; no new runtime or experimental claims.',
    'manual_report':'proposal-structure-review-20260914.md', 'raw_root':str(raw.relative_to(ROOT)),
    'seven_chapters_per_language':True,'rq_text_unchanged':True,'sixteen_included_modules_byte_equal':True,
    'non_timeline_tables_byte_equal':True,'experiment_slides':experiments,
    'mirror_text_equality':True,'abstract_keywords_one_page':True,'eight_final_logs_clean':True,
    'pages':{n:len(d) for n,d in docs.items()},'slide_pages':slide_pages,'editable_text':editable,
    'pptx_slides_and_notes':49,'libreoffice_pages':49,'text_outside_page':False,
    'source_sha256':{str(p.relative_to(BASE)):sha(p) for p in source_paths},
    'artifact_sha256':{n:sha(BASE/n) for n in names+['slides/NDNSF_proposal_hybrid_editable.pptx']},
    'client_limit':'LibreOffice rendering; no Microsoft PowerPoint or Google Slides client test.',
}
(BASE/'proposal-structure-validation-20260914.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
print(json.dumps({k:report[k] for k in ['verdict','pages','pptx_slides_and_notes','editable_text']},ensure_ascii=False))
