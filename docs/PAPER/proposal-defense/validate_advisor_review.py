#!/usr/bin/env python3
"""Audit document correspondence and artifact integrity, not scientific truth."""
import argparse
import hashlib
import json
import re
import subprocess
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

import fitz

BASE = Path(__file__).resolve().parent
ROOT = BASE.parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--raw', type=Path, required=True)
parser.add_argument('--base-ref', required=True)
args = parser.parse_args()
RAW = args.raw.resolve()

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def old(path):
    return subprocess.check_output(['git','show',f'{args.base_ref}:{path.relative_to(ROOT)}'],cwd=ROOT,text=True)

def frames(text):
    return dict(re.findall(r'\\begin\{frame\}\{([^\n]+)\}\n(.*?)\\end\{frame\}',text,re.S))

pdf_names=['main.pdf','en/main.pdf','main_ch.pdf','ch/main.pdf',
           'slides/main.pdf','slides/main_35min.pdf','slides/speaker_notes.pdf','slides/speaker_notes_35min.pdf']
docs={n:fitz.open(BASE/n) for n in pdf_names}
texts={n:[p.get_text() for p in d] for n,d in docs.items()}
for a,b in zip(pdf_names[::2],pdf_names[1::2]):
    assert texts[a]==texts[b],(a,b)
for n in ['main.pdf','main_ch.pdf']:
    assert 'Keywords:' in texts[n][1] and 'Table of Contents' in texts[n][2],n

outside={}
for name,doc in docs.items():
    errors=[]
    for i,page in enumerate(doc):
        for b in page.get_text('dict')['blocks']:
            for line in b.get('lines',[]):
                for s in line['spans']:
                    x0,y0,x1,y1=s['bbox']
                    if s['text'].strip() and (x0 < -1 or y0 < -1 or x1 > page.rect.width+1 or y1 > page.rect.height+1):
                        errors.append({'page':i+1,'text':s['text']})
    outside[name]=errors
    assert not errors,(name,errors)

logs=[RAW/'r3'/tag/(name+'.log') for tag,name in [('root-en','main'),('root-ch','main_ch'),('en','main'),('ch','main')]]
logs += [RAW/'r4'/tag/(name+'.log') for tag,name in [('slides','main'),('slides35','main_35min'),('speaker_notes','speaker_notes'),('speaker_notes_35min','speaker_notes_35min')]]
for path in logs:
    assert not re.search(r'Overfull|Missing character|undefined|^!',path.read_text(errors='replace'),re.M),path

modules=['application-motivation','ndn-background','framework-architecture','invocation-data-services','uav-workflows','di-workflows','evaluation-methods']
preservation=[]
for lang in ['en','ch']:
    path=BASE/lang/'chapters/research-revision.tex'
    prior,current=old(path),path.read_text()
    assert re.findall(r'\\(?:chapter|section|subsection)\{[^}]+\}',prior)==re.findall(r'\\(?:chapter|section|subsection)\{[^}]+\}',current)
    for module in modules:
        marker='\\input{\\proposalroot/'+lang+'/chapters/'+module+'}'
        assert prior.count(marker)==current.count(marker)==1
        mp=BASE/lang/'chapters'/(module+'.tex')
        if module!='application-motivation':assert old(mp)==mp.read_text(),mp
    # The entire evaluation/timeline/conclusion tail is preserved byte-for-byte.
    heading='\\chapter{Available Evidence and Required Evaluation}' if lang=='en' else '\\chapter{已有证据与待完成评价}'
    assert heading in prior and prior.split(heading,1)[1]==current.split(heading,1)[1]
    preservation.append({'language':lang,'all_headings_and_seven_modules_retained':True,
                         'six_unchanged_modules_byte_equal':True,'evaluation_timeline_conclusion_byte_equal':True})

slide_source=BASE/'slides/research-slides.tex'
before,after=frames(old(slide_source)),frames(slide_source.read_text())
experiment_titles=[t for t in before if t.startswith(('Mobility:', 'Fewer Execution Starts', 'A Useful Negative Result', 'Backup:'))]
assert len(experiment_titles)==6
for title in experiment_titles:assert before[title]==after[title],title
source_refs=set(re.findall(r'\\refentry\{(\d+)\}',slide_source.read_text()))
body=slide_source.read_text().split('\\begin{frame}{References:')[0]
cited={n for group in re.findall(r'\[([\d,]+)\]',body) for n in group.split(',')}
assert cited==source_refs,(cited-source_refs,source_refs-cited)

slide_pages=[]
for i,page in enumerate(docs['slides/main.pdf']):
    text=page.get_text(clip=fitz.Rect(0,0,page.rect.width,page.rect.height*.94))
    title=text.splitlines()[0]
    words=len(text.split())
    if not title.startswith('References:'):assert words<=100,(i+1,words)
    slide_pages.append({'page':i+1,'title':title,'words':words})

ns={'p':'http://schemas.openxmlformats.org/presentationml/2006/main',
    'a':'http://schemas.openxmlformats.org/drawingml/2006/main'}
original_pptx=BASE.parent/'NDNSF_proposal_reviewed.pptx'
ppt_comments=[]
with zipfile.ZipFile(original_pptx) as z:
    for i in range(1,22):
        node=ET.fromstring(z.read(f'ppt/slides/slide{i}.xml'))
        for sp in node.findall('.//p:sp',ns):
            if any(c.get('val','').upper() in ('FF0000','C00000') for c in sp.findall('.//a:srgbClr',ns)):
                text=' '.join(t.text or '' for t in sp.findall('.//a:t',ns)).strip()
                if text:ppt_comments.append({'id':f'C{len(ppt_comments)+1:02d}','page':i,'text':text})
assert len(ppt_comments)==26,len(ppt_comments)
original_pdf=BASE.parent/'reference-pdfs/proposal 2.pdf'
pdf_comments=[]
with fitz.open(original_pdf) as original:
    for i,page in enumerate(original):
        for b in page.get_text('dict')['blocks']:
            text=' '.join(s['text'] for l in b.get('lines',[]) for s in l['spans'] if s['color'] in (16721408,0xff0000)).strip()
            if text:pdf_comments.append({'id':f'P{len(pdf_comments)+1:02d}','page':i+1,'text':text})
assert len(pdf_comments)==19,len(pdf_comments)

pptx=BASE/'slides/NDNSF_proposal_hybrid_editable.pptx'
with zipfile.ZipFile(pptx) as z:
    assert len([p for p in z.namelist() if re.fullmatch(r'ppt/slides/slide\d+.xml',p)])==len(slide_pages)
    assert len([p for p in z.namelist() if re.fullmatch(r'ppt/notesSlides/notesSlide\d+.xml',p)])==len(slide_pages)
conv=json.loads((BASE/'slides/build/advisor-review-20260914-r3/editable-text-manifest.json').read_text())['validation']
assert conv['source_span_count']==conv['assigned_once_count']
assert conv['background_extractable_text_characters']==0
lo=fitz.open(RAW/'lo-final/NDNSF_proposal_hybrid_editable.pdf')
assert len(lo)==len(slide_pages)
lo_report=json.loads((RAW/'lo-render/render-audit.json').read_text())[0]
assert not any(p['text_outside_page'] for p in lo_report['pages'])

ledger=json.loads((BASE/'sentence-claim-ledger-20260914.json').read_text())
for name,value in ledger['source_sha256'].items():assert sha(BASE/name)==value,name
report={
 'date':'2026-09-14','verdict':'DOCUMENT_CHECKS_PASS_RESEARCH_GAPS_REMAIN',
 'base_ref':args.base_ref,'raw_root':str(RAW.relative_to(ROOT)),
 'manual_report':'advisor-review-20260914.md',
 'review_scope':'English complete active prose and slide claim units; changed Chinese passages aligned; unchanged Chinese modules retain prior review.',
 'teacher_originals':{str(p.relative_to(BASE.parent)):sha(p) for p in [original_pdf,original_pptx]},
 'pdf_comments':pdf_comments,'pptx_comments':ppt_comments,
 'preservation':preservation,'experiment_slides_byte_equal':experiment_titles,
 'pages':{n:len(d) for n,d in docs.items()},'mirror_text_equality':True,
 'abstract_keywords_one_page':True,'latex_logs_no_errors_overfull_missing_glyphs_undefined':True,
 'text_outside_page':outside,'slide_pages':slide_pages,'slide_citation_ids':sorted(cited,key=int),
 'editable_text':conv,'pptx_notes_count':len(slide_pages),'libreoffice_pages':len(lo),
 'claim_inventory_units':len(ledger['units']),'source_sha256':ledger['source_sha256'],
 'artifact_sha256':{n:sha(BASE/n) for n in pdf_names+['slides/NDNSF_proposal_hybrid_editable.pptx']},
 'client_limit':'LibreOffice export and screenshots; Microsoft PowerPoint and Google Slides clients not run.',
 'research_status':'No new experiments; literature novelty, full alternatives, collaboration qualification and historical DI provenance remain open.'}
(BASE/'advisor-review-validation-20260914.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
print(json.dumps({k:report[k] for k in ['verdict','pages','claim_inventory_units','pptx_notes_count','libreoffice_pages']},ensure_ascii=False))
