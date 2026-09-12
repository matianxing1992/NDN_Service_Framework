#!/usr/bin/env python3
"""Inventory canonical DI declarations and export seams without importing DI."""
import ast
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
FEATURE = Path(__file__).resolve().parents[1]
CPP = ROOT / 'NDNSF-DistributedInference/cpp'
PY = ROOT / 'NDNSF-DistributedInference/ndnsf_distributed_inference'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def group(path):
    name = Path(path).stem
    if '/adapters/' in path or '/sdk/' in path:
        return 'extension-model'
    if any(x in path for x in ['/compatibility/', '/experimental/']):
        return 'compatibility-experimental'
    if '/app_sdk/' in path or '/api/' in path:
        return 'application-sdk'
    if '/security/' in path or any(x in name for x in ['Grant', 'Protected', 'Security', 'Authority', 'Admission', 'CanonicalJson']):
        return 'security-authority'
    if any(x in name.lower() for x in ['conversation', 'checkpoint', 'journal', 'recovery']):
        return 'conversation-persistence'
    if '/planner/' in path or any(x in name for x in ['Planning', 'Planner', 'Placement', 'Catalog', 'Preparation', 'Graph']):
        return 'planning-preparation'
    if '/ops/' in path or any(x in name.lower() for x in ['operation', 'release', 'evidence', 'gui', 'probe', 'fault']):
        return 'operations-diagnostics'
    if any(x in name.lower() for x in ['provider', 'runner', 'assembly', 'assembler', 'materializer', 'backend', 'executor']):
        return 'provider-execution'
    if any(x in name.lower() for x in ['client', 'request', 'application']):
        return 'request-client'
    return 'runtime-contracts'


def py_record(path):
    text = path.read_text()
    lines = text.splitlines()
    tree = ast.parse(text)
    entries, imports, exports = [], [], []

    def walk(nodes, scope=()):
        for node in nodes:
            if isinstance(node, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
                if node.name.startswith('_') and node.name not in ('__init__', '__enter__', '__exit__', '__await__', '__aiter__', '__anext__'):
                    continue
                # Keep exact source header through its first body statement.
                end = node.body[0].lineno - 1 if node.body else node.lineno
                signature = '\n'.join(lines[node.lineno-1:max(node.lineno, end)]).strip()
                entries.append(dict(name='.'.join(scope+(node.name,)), line=node.lineno,
                    kind='class' if isinstance(node, ast.ClassDef) else 'function',
                    signature=signature, documented=bool(ast.get_docstring(node))))
                if isinstance(node, ast.ClassDef):
                    walk(node.body, scope+(node.name,))
            elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name) and not node.target.id.startswith('_'):
                entries.append(dict(name='.'.join(scope+(node.target.id,)), line=node.lineno,
                    kind='field', signature=ast.get_source_segment(text, node), documented=False))
    walk(tree.body)
    for node in ast.walk(tree):
        if isinstance(node, ast.ImportFrom):
            imports.append(dict(line=node.lineno, module='.'*node.level+(node.module or ''),
                                names=[dict(name=n.name, alias=n.asname) for n in node.names]))
        if isinstance(node, ast.Assign) and any(isinstance(n, ast.Name) and n.id=='__all__' for n in node.targets):
            try:
                exports.append(ast.literal_eval(node.value))
            except (ValueError, TypeError):
                exports.append({'dynamic': ast.get_source_segment(text, node.value), 'line': node.lineno})
    return dict(file=str(path.relative_to(ROOT)), entries=entries, imports=imports,
                exports=exports, parse_errors=[])


def main():
    headers = sorted(p for p in CPP.rglob('*') if p.suffix in ('.hpp', '.h') and 'vendor' not in p.relative_to(CPP).parts)
    python = sorted(PY.rglob('*.py'))
    seams = [ROOT/'pythonWrapper/src/ndnsf/di_bindings.cpp', ROOT/'pythonWrapper/src/ndnsf/_ndnsf.cpp',
             ROOT/'wscript', ROOT/'pythonWrapper/setup.py',
             PY/'compatibility/manifest.json']
    seams = [p for p in seams if p.exists()]
    inputs = headers+python+seams
    before = {str(p.relative_to(ROOT)): digest(p) for p in inputs}
    parsed = json.loads(subprocess.check_output(['node', str(ROOT/'Design/extract-cpp-api.cjs')],
        input=json.dumps([str(p.relative_to(ROOT)) for p in headers]), text=True, cwd=ROOT))
    parsed.extend(py_record(p) for p in python)
    for record in parsed:
        record['sha256'] = before[record['file']]
        record['family'] = group(record['file'])
        record['review'] = 'DECLARATION_INVENTORY; semantic findings are family-scoped in api-review.md'
        for entry in record['entries']:
            entry.pop('documentation', None)
            entry['id'] = hashlib.sha256((record['file']+':'+entry['name']+':'+entry['signature']).encode()).hexdigest()[:16]
    bindings = []
    for path in seams:
        if path.suffix != '.cpp':
            continue
        for line, text in enumerate(path.read_text().splitlines(), 1):
            if re.search(r'py::(?:class_|enum_)|\.def(?:_\w+)?\(|\.attr\(', text):
                bindings.append(dict(file=str(path.relative_to(ROOT)), line=line, declaration=text.strip()))
    after_headers = sorted(p for p in CPP.rglob('*') if p.suffix in ('.hpp', '.h') and 'vendor' not in p.relative_to(CPP).parts)
    if headers != after_headers or python != sorted(PY.rglob('*.py')) or before != {str(p.relative_to(ROOT)): digest(p) for p in inputs}:
        raise RuntimeError('Source changed during inventory; refusing publication')
    report = dict(source_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        scope='All project-owned canonical DI C++ headers (excluding vendor) and Python package files; DI binding TU plus shared Core binding export candidates and installation seams. Syntax only, no macro/inheritance expansion or import execution. Binding candidates are not a count of DI exports.',
        extractor_sha256=digest(ROOT/'Design/extract-cpp-api.cjs'), generator_sha256=digest(Path(__file__)),
        input_sha256=before, files=parsed, binding_candidates=bindings)
    errors = [dict(file=r['file'], errors=r['parse_errors']) for r in parsed if r['parse_errors']]
    if errors:
        raise RuntimeError('Unparsed project declarations; refusing publication: '+str(errors))
    dest = FEATURE/'evidence/api-inventory.json'
    if '--check' in sys.argv:
        old = json.loads(dest.read_text())
        # A documentation commit may change HEAD without changing any inputs.
        report['source_commit'] = old['source_commit']
        if old != report:
            raise SystemExit('API_INVENTORY_STALE')
    else:
        dest.write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')
    print(json.dumps(dict(headers=len(headers), python_files=len(python), declarations=sum(len(r['entries']) for r in parsed),
        binding_candidates=len(bindings), parse_errors=errors), ensure_ascii=False))


if __name__ == '__main__':
    main()
