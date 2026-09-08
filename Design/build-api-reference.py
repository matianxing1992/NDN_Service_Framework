#!/usr/bin/env python3
"""Generate source-bound API inventories without importing product modules."""
from pathlib import Path
import ast, hashlib, io, json, os, subprocess, tokenize

root = Path(__file__).resolve().parent.parent
design = root / 'Design'
prefixes = {'Core': ('ndn-service-framework/', 'pythonWrapper/ndnsf/'),
            'Repo': ('NDNSF-DistributedRepo/include/', 'NDNSF-DistributedRepo/pythonWrapper/'),
            'DI': ('NDNSF-DistributedInference/cpp/', 'NDNSF-DistributedInference/ndnsf_distributed_inference/'),
            'UAV': ('NDNSF-UAV-APP/',)}
tracked = subprocess.check_output(['git','ls-files','-z'], cwd=root).decode().split('\0')
from design_state import api_files
files = api_files(root)

cpp = sorted(p for p in files if not p.endswith('.py'))
parsed = json.loads(subprocess.check_output(['node',str(design/'extract-cpp-api.cjs')], input=json.dumps(cpp), text=True, cwd=root))
by_file = {f['file']: f for f in parsed}

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

inventory=dict(revision='R1',baseline_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
    scope='C++ canonical headers: public/protected declarations and fields; Python canonical packages: public-name definitions/annotated fields and literal exports. No macro expansion, inherited-member expansion or ABI/runtime qualification.',
    files=[by_file[p] for p in sorted(by_file)])
dest=design/'api';dest.mkdir(exist_ok=True)
(dest/'inventory.json').write_text(json.dumps(inventory,ensure_ascii=False,indent=2)+'\n')
for module in prefixes:
    parts=['# '+module+' API 参考\n\n声明从源码语法树提取。保留准确类型、参数、默认值、限定符和原始注释；注释不代替运行证据。中文语义契约见开发者指南。protected 扩展点、测试 helper、应用内部接口各自标注。\n']
    for f in inventory['files']:
        if f['module']!=module: continue
        parts.append('\n## '+f['file']+'\n\n源码 SHA-256：`'+f['sha256']+'`。\n')
        if f.get('exports'):parts.append('\n显式导出：`'+'`, `'.join(f['exports'])+'`。\n')
        if f['parse_errors']:parts.append('\n解析边界：'+json.dumps(f['parse_errors'],ensure_ascii=False)+'\n')
        for e in f['entries']:
            parts.append('\n### '+e['id']+' · '+e['name'].replace('\n',' ')+'\n\n'+e['access']+' / '+e['surface']+'；[源码](../../'+f['file']+'#L'+str(e['line'])+')\n\n```'+('python' if f['file'].endswith('.py') else 'cpp')+'\n'+e['signature']+'\n```\n')
            if e.get('documentation'): parts.append('\n原始接口说明：\n\n```text\n'+e['documentation']+'\n```\n')
    (dest/(module.lower()+'-reference.md')).write_text('\n'.join(line.rstrip() for line in ''.join(parts).splitlines())+'\n')
errors=[(f['file'],f['parse_errors']) for f in inventory['files'] if f['parse_errors']]
print(json.dumps({'files':len(inventory['files']),'entries':sum(len(f['entries']) for f in inventory['files']),'per_module':{m:sum(len(f['entries']) for f in inventory['files'] if f['module']==m) for m in prefixes},'parse_errors':errors},ensure_ascii=False))

subprocess.run(['python3',str(design/'render-api-contracts.py')], check=True)
subprocess.run(['node',str(design/'extract-python-bindings.cjs')],cwd=root,check=True)
