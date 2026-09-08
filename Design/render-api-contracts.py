#!/usr/bin/env python3
"""Render reviewed contracts using captured exact API declarations."""
from pathlib import Path
import json, re, sys
design = Path(__file__).resolve().parent
dest = design / 'api'
target='--target' in sys.argv
inventory = json.loads((dest / ('target-inventory.json' if target else 'inventory.json')).read_text())

def tex(text):
    def escape(value):
        return ''.join({'\\':r'\textbackslash{}','&':r'\&','%':r'\%','$':r'\$','#':r'\#','_':r'\_','{':r'\{','}':r'\}','~':r'\textasciitilde{}','^':r'\textasciicircum{}'}.get(c,c) for c in value)
    pieces=re.split(r'([A-Za-z][A-Za-z0-9_:/.-]{23,})',text)
    return ''.join(r'\code{'+part+'}' if i%2 else escape(part) for i,part in enumerate(pieces))

target='--target' in sys.argv
cards=json.loads((design/('target-api-contracts.json' if target else 'api-contracts.json')).read_text())['cards']
parts=['\\clearpage\n']; coverage=[]
for card in cards:
    parts.append('\n\\section{'+tex(card['title'])+'}\n\\lead{'+card['id']+' · '+tex(card['module'])+' 接口契约}\n')
    selected=[]
    for selector in card['selectors']:
        suffix,name,*choice=selector
        hits=[(f,e) for f in inventory['files'] if f['file'].endswith('/'+suffix) for e in f['entries'] if e['kind']=='function' and e['name'].endswith(name)]
        if not hits: raise ValueError('Missing API selector: '+repr(selector))
        if choice: hits=[hits[choice[0]]]
        for f,e in hits:
            parts.append('\n\\textbf{'+e['id']+'}\\quad '+tex(e['access'])+'\\par\n\\begin{Verbatim}[fontsize=\\footnotesize,breaklines,breakanywhere]\n'+e['signature']+'\n\\end{Verbatim}\n\\textbf{源码：}\\path{'+f['file']+'}，第 '+str(e['line'])+' 行。\\par\n')
            selected.append(e['id'])
    for signature in card.get('planned_signatures',[]):
        if not target: raise ValueError('Planned API cannot be rendered as current implementation')
        parts.append('\n\\textbf{目标接口：PLANNED}\\par\n\\begin{Verbatim}[fontsize=\\footnotesize,breaklines,breakanywhere]\n'+signature+'\n\\end{Verbatim}\n')
    for section in card['sections']:
        parts.append('\n\\subsection{'+tex(section['title'])+'}\n'+tex(section['text'])+'\n')
    reference = 'target-inventory.json' if target else card['module'].lower()+'-reference.md'
    parts.append('\n完整声明查询：\\path{api/'+reference+'}。\n')
    coverage.append(dict(contract=card['id'],title=card['title'],api_ids=selected))
(design/('target-api.tex' if target else 'current-api.tex')).write_text(''.join(parts))
(dest/('target-contract-map.json' if target else 'contract-map.json')).write_text(json.dumps(coverage,ensure_ascii=False,indent=2)+'\n')
print('Rendered',len(cards),'contracts;',sum(len(c['api_ids']) for c in coverage),'signature examples;', 'target' if target else 'current')
