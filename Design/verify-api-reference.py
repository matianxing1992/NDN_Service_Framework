#!/usr/bin/env python3
"""Check documented declarations, reviewed selectors, and snapshot coverage."""
from pathlib import Path
import hashlib, json, re, subprocess, tempfile, shutil, sys

root=Path(__file__).resolve().parent.parent
inventory=json.loads((root/'Design/api/inventory.json').read_text())
snapshot=json.loads((root/'Design/source-baseline.json').read_text())
normalize=lambda s:re.sub(r'\s+',' ',s).strip()
errors=[];ids=set();entries=0
generator_hash=hashlib.sha256(b''.join((root/'Design'/name).read_bytes() for name in
    ('build-api-reference.py','extract-cpp-api.cjs','design_state.py'))).hexdigest()
if inventory.get('generator_sha256')!=generator_hash:
    errors.append('API generator identity changed: regenerate inventory')
from design_state import api_files, source_files
actual=set(api_files(root)); captured={f['file'] for f in inventory['files']}
if actual != captured: errors.append('API file set changed: '+repr(sorted(actual ^ captured)))
missing=source_files(root)-set(snapshot['files'])
if missing: errors.append('implementation coverage missing: '+repr(sorted(missing)))
for name, metadata in snapshot['files'].items():
    path=root/name
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=metadata['sha256']:
        errors.append('snapshot source drift: '+name)
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
coverage=json.loads((root/'Design/api/behavior-coverage.json').read_text())['entries']
target_snapshot=json.loads((root/'Design/target-source-baseline.json').read_text())
for f in json.loads((root/'Design/api/target-inventory.json').read_text())['files']:
    if target_snapshot['files'].get(f['file'],{}).get('sha256') != f['sha256']:
        errors.append('target inventory differs from frozen baseline: '+f['file'])
function_ids={e['id'] for f in inventory['files'] for e in f['entries'] if e['kind']=='function'}
if {e['api_id'] for e in coverage} != function_ids or len(coverage)!=len(function_ids):
    errors.append('behavior coverage missing, duplicated or stale')
for e in coverage:
    if e['status'] not in {'SIGNATURE_ONLY','CONTRACT_REFERENCED'}:
        errors.append('unknown behavior status: '+e['api_id'])
# Re-render in isolation: stale TeX/maps and unresolved frozen target selectors fail.
with tempfile.TemporaryDirectory(prefix='design-render-check-') as temporary:
    work=Path(temporary); (work/'api').mkdir()
    for name in ('render-api-contracts.py','render-api-reference.py','api-contracts.json','target-api-contracts.json'):
        shutil.copyfile(root/'Design'/name, work/name)
    for name in ('inventory.json','target-inventory.json'):
        shutil.copyfile(root/'Design/api'/name,work/'api'/name)
    for target in (False,True):
        completed=subprocess.run([sys.executable,str(work/'render-api-contracts.py')]+(['--target'] if target else []), capture_output=True,text=True)
        if completed.returncode:
            errors.append('contract render failed: '+completed.stderr[-500:]); continue
        for name in (('target-api.tex','api/target-contract-map.json') if target else ('current-api.tex','api/contract-map.json')):
            if (work/name).read_bytes() != (root/'Design'/name).read_bytes():
                errors.append('stale generated contract: '+name)
        completed=subprocess.run([sys.executable,str(work/'render-api-reference.py')]+(['--target'] if target else []),capture_output=True,text=True)
        if completed.returncode:
            errors.append('reference render failed: '+completed.stderr[-500:]); continue
        for module in ('core','repo','di','uav'):
            name='api/'+('target-' if target else '')+module+'-reference.md'
            path=root/'Design'/name
            if not path.is_file() or (work/name).read_bytes()!=path.read_bytes():
                errors.append('stale generated reference: '+name)
bindings=json.loads((root/'Design/api/python-bindings.json').read_text())
for f in bindings:
    data=(root/f['file']).read_bytes()
    if hashlib.sha256(data).hexdigest()!=f['sha256']:errors.append('binding source drift: '+f['file'])
    committed=hashlib.sha256(subprocess.check_output(['git','show',f['baseline_commit']+':'+f['file']],cwd=root)).hexdigest()
    if f['matches_commit'] != (committed == f['sha256']): errors.append('binding commit identity flag mismatch: '+f['file'])
    if snapshot['files'].get(f['file'],{}).get('sha256')!=f['sha256']:
        errors.append('binding snapshot mismatch: '+f['file'])
result=dict(result='FAIL' if errors else 'PASS',files=len(inventory['files']),entries=entries,bindings=sum(len(f['entries']) for f in bindings),
            functions=sum(e['kind']=='function' for f in inventory['files'] for e in f['entries']),errors=errors)
print(json.dumps(result,ensure_ascii=False))
raise SystemExit(bool(errors))
