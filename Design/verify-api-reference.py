#!/usr/bin/env python3
"""Check documented declarations, reviewed selectors, and snapshot coverage."""
from pathlib import Path
import hashlib, json, re, subprocess

root=Path(__file__).resolve().parent.parent
inventory=json.loads((root/'Design/api/inventory.json').read_text())
snapshot=json.loads((root/'Design/source-baseline.json').read_text())
normalize=lambda s:re.sub(r'\s+',' ',s).strip()
errors=[];ids=set();entries=0
for f in inventory['files']:
    source=(root/f['file']).read_text();digest=hashlib.sha256(source.encode()).hexdigest()
    if digest!=f['sha256']:errors.append('source drift: '+f['file'])
    if snapshot['files'].get(f['file'],{}).get('sha256')!=f['sha256']:errors.append('snapshot mismatch: '+f['file'])
    if f['parse_errors']:errors.append('parse errors: '+f['file'])
    normalized=normalize(source)
    for e in f['entries']:
        entries+=1;ids.add(e['id'])
        if normalize(e['signature']) not in normalized:errors.append('signature mismatch: '+e['id']+' '+f['file'])
for card in json.loads((root/'Design/api/contract-map.json').read_text()):
    for identifier in card['api_ids']:
        if identifier not in ids:errors.append('unknown contract API: '+identifier)
bindings=json.loads((root/'Design/api/python-bindings.json').read_text())
for f in bindings:
    data=(root/f['file']).read_bytes()
    if hashlib.sha256(data).hexdigest()!=f['sha256']:errors.append('binding source drift: '+f['file'])
    if hashlib.sha256(subprocess.check_output(['git','show',f['baseline_commit']+':'+f['file']],cwd=root)).hexdigest()!=f['sha256']:errors.append('binding commit mismatch: '+f['file'])
result=dict(result='FAIL' if errors else 'PASS',files=len(inventory['files']),entries=entries,bindings=sum(len(f['entries']) for f in bindings),
            functions=sum(e['kind']=='function' for f in inventory['files'] for e in f['entries']),errors=errors)
print(json.dumps(result,ensure_ascii=False))
raise SystemExit(bool(errors))
