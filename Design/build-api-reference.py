#!/usr/bin/env python3
"""Generate source-bound API inventories without importing product modules."""
from pathlib import Path
import ast, hashlib, io, json, os, subprocess, tokenize, sys

root = Path(__file__).resolve().parent.parent
design = root / 'Design'
prefixes = {'Core': ('ndn-service-framework/', 'pythonWrapper/ndnsf/'),
            'Repo': ('NDNSF-DistributedRepo/include/', 'NDNSF-DistributedRepo/pythonWrapper/'),
            'DI': ('NDNSF-DistributedInference/cpp/', 'NDNSF-DistributedInference/ndnsf_distributed_inference/'),
            'UAV': ('NDNSF-UAV-APP/',)}
tracked = subprocess.check_output(['git','ls-files','-z'], cwd=root).decode().split('\0')
from design_state import api_files
files = api_files(root)
initial_hashes = {p: hashlib.sha256((root/p).read_bytes()).hexdigest() for p in files}
generator_hash = hashlib.sha256(b''.join((design/name).read_bytes() for name in
    ('build-api-reference.py', 'extract-cpp-api.cjs', 'design_state.py'))).hexdigest()
previous = {}
if '--changed-only' in sys.argv:
    captured = design/'api/inventory.json'
    if captured.exists():
        old_inventory = json.loads(captured.read_text())
        if old_inventory.get('generator_sha256') == generator_hash:
            previous = {f['file']: f for f in old_inventory['files']
                        if f['file'] in files and f['module'] == files[f['file']]}
changed = {p for p in files if p not in previous or previous[p]['sha256'] != initial_hashes[p]}

cpp = sorted(p for p in changed if not p.endswith('.py'))
parsed = json.loads(subprocess.check_output(['node',str(design/'extract-cpp-api.cjs')], input=json.dumps(cpp), text=True, cwd=root))
by_file = {p: f for p, f in previous.items() if p not in changed}
by_file.update({f['file']: f for f in parsed})

def header(source, node):
    lines = source.splitlines(True)
    text = ''.join(lines[node.lineno-1:])
    # Remove indentation only on the first line; token positions address this text.
    text = text[node.col_offset:]
    depth = 0
    for token in tokenize.generate_tokens(io.StringIO(text).readline):
        if token.type == tokenize.OP:
            if token.string in ('(', '[', '{'): depth += 1
            elif token.string in (')', ']', '}'): depth -= 1
            elif token.string == ':' and depth == 0:
                return ''.join(text.splitlines(True)[:token.end[0]-1]) + text.splitlines(True)[token.end[0]-1][:token.end[1]]
    raise ValueError('Missing signature: ' + str(node.lineno))

for p in sorted(files):
    if p not in changed: continue
    source = (root/p).read_text()
    if p.endswith('.py'):
        tree = ast.parse(source); entries=[]; exports=[]
        def visit(body, scope=[]):
            for n in body:
                if isinstance(n,(ast.ClassDef,ast.FunctionDef,ast.AsyncFunctionDef)) and (not n.name.startswith('_') or n.name=='__init__'):
                    entries.append(dict(name='.'.join(scope+[n.name]), kind='class' if isinstance(n,ast.ClassDef) else 'function', access='public-by-name', line=n.lineno, signature=header(source,n), documentation=ast.get_docstring(n,clean=True) or '', test_only='test' in n.name.lower()))
                    if isinstance(n,ast.ClassDef): visit(n.body,scope+[n.name])
                elif isinstance(n,ast.AnnAssign) and isinstance(n.target,ast.Name) and not n.target.id.startswith('_'):
                    entries.append(dict(name='.'.join(scope+[n.target.id]),kind='field',access='public-by-name',line=n.lineno,signature=ast.get_source_segment(source,n),documentation='',test_only=False))
                elif isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='__all__' for t in n.targets):
                    try: exports.extend(ast.literal_eval(n.value))
                    except (ValueError,TypeError): exports.append('<dynamic __all__: inspect source>')
        visit(tree.body)
        by_file[p]=dict(file=p, entries=entries,parse_errors=[],exports=exports)
    record=by_file[p];record.update(module=files[p],sha256=hashlib.sha256((root/p).read_bytes()).hexdigest())
    for e in record['entries']:
        identity=p+':'+e['name']+':'+e['signature']
        e['id']='API-'+hashlib.sha256(identity.encode()).hexdigest()[:12]
        e['surface']='test-helper' if e.get('test_only') else ('application-internal' if files[p]=='UAV' or '/detail/' in p or '/experimental/' in p else 'declared-interface')

if files != api_files(root) or any(hashlib.sha256((root/p).read_bytes()).hexdigest()!=initial_hashes[p] for p in files):
    raise RuntimeError('API source changed during extraction; no inventory published')
inventory=dict(revision='R3',baseline_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
    generator_sha256=generator_hash,
    scope='C++ canonical headers: public/protected declarations and fields; Python canonical packages: public-name definitions/annotated fields and literal exports. No macro expansion, inherited-member expansion or ABI/runtime qualification.',
    files=[by_file[p] for p in sorted(by_file)])
dest=design/'api';dest.mkdir(exist_ok=True)
(dest/'inventory.json').write_text(json.dumps(inventory,ensure_ascii=False,indent=2)+'\n')
subprocess.run(['python3',str(design/'render-api-reference.py')], check=True)
errors=[(f['file'],f['parse_errors']) for f in inventory['files'] if f['parse_errors']]
print(json.dumps({'files':len(inventory['files']),'entries':sum(len(f['entries']) for f in inventory['files']),'per_module':{m:sum(len(f['entries']) for f in inventory['files'] if f['module']==m) for m in prefixes},'parse_errors':errors},ensure_ascii=False))

subprocess.run(['python3',str(design/'render-api-contracts.py')], check=True)
subprocess.run(['node',str(design/'extract-python-bindings.cjs')],cwd=root,check=True)
