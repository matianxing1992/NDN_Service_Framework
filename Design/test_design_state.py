"""Regression checks for omissions, stale PDFs and target snapshot coupling."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from design_state import api_files, source_files, build_inputs, digest, verify_provenance


class DesignStateTests(unittest.TestCase):
    def test_incremental_matches_full_and_rejects_capture_race(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); design=root/'Design'; design.mkdir()
            for name in ('build-api-reference.py','design_state.py'):
                shutil.copyfile(Path(__file__).with_name(name),design/name)
            # Only Python parser inputs: mock external C++/output tools, not AST extraction.
            extractor=design/'extract-cpp-api.cjs'
            extractor.write_text('process.stdout.write("[]");')
            (design/'extract-python-bindings.cjs').write_text('process.stdout.write("[]");')
            for name in ('render-api-reference.py','render-api-contracts.py'):
                (design/name).write_text('pass\n')
            package=root/'NDNSF-DistributedInference/ndnsf_distributed_inference'
            package.mkdir(parents=True)
            a=package/'a.py'; b=package/'b.py'; c=package/'c.py'
            a.write_text('def original(x: int = 1):\n    return x\n')
            b.write_text('def removed():\n    return None\n')
            subprocess.run(['git','init','-q',d],check=True)
            subprocess.run(['git','add',str(a),str(b)],cwd=root,check=True)
            subprocess.run(['git','-c','user.name=Fixture','-c','user.email=fixture@example.invalid',
                            'commit','-qm','fixture'],cwd=root,check=True)
            command=['python3',str(design/'build-api-reference.py')]
            subprocess.run(command,check=True,capture_output=True)
            a.write_text('def changed(x: str = "v"):\n    return x\n')
            c.write_text('class Added:\n    field: int = 2\n')
            subprocess.run(['git','rm','-q',str(b)],cwd=root,check=True)
            subprocess.run(['git','add',str(c)],cwd=root,check=True)
            subprocess.run(command+['--changed-only'],check=True,capture_output=True)
            output=design/'api/inventory.json'
            incremental=json.loads(output.read_text())['files']
            subprocess.run(command,check=True,capture_output=True)
            self.assertEqual(incremental,json.loads(output.read_text())['files'])
            self.assertEqual({f['file'] for f in incremental},
                             {str(a.relative_to(root)),str(c.relative_to(root))})
            before=output.read_bytes()
            # A change after initial hashing must not publish a mixed inventory.
            extractor.write_text('require("fs").appendFileSync('+json.dumps(str(a))+
                                 ', "\\n# changed during capture\\n"); process.stdout.write("[]");')
            result=subprocess.run(command+['--changed-only'],capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('API source changed during extraction',result.stderr)
            self.assertEqual(before,output.read_bytes())

    def test_duplicate_declarations_keep_locations(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); (root/'api').mkdir()
            shutil.copyfile(Path(__file__).with_name('build-behavior-coverage.py'),root/'coverage.py')
            entries=[dict(id='API-1',name='f',kind='function',line=n) for n in (2,20)]
            (root/'api/inventory.json').write_text(json.dumps({'files':[{'file':'a.hpp','entries':entries}]}))
            (root/'api/contract-map.json').write_text('[]')
            subprocess.run(['python3',str(root/'coverage.py')],check=True,capture_output=True)
            result=json.loads((root/'api/behavior-coverage.json').read_text())['entries']
            self.assertEqual(len(result),1)
            self.assertEqual(result[0]['declaration_lines'],[2,20])

    def test_new_implementation_and_api_are_detected(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            subprocess.run(['git', 'init', '-q', d], check=True)
            for name in ('New.cpp', 'New.hpp'):
                p=root/'NDNSF-DistributedInference/cpp'/name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text('// fixture\n')
            subprocess.run(['git', 'add', 'NDNSF-DistributedInference/cpp/New.cpp',
                            'NDNSF-DistributedInference/cpp/New.hpp'], cwd=root, check=True)
            self.assertEqual(len(source_files(root)), 2)
            self.assertEqual(list(api_files(root)), ['NDNSF-DistributedInference/cpp/New.hpp'])

    def test_changed_tex_or_pdf_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); tex=root/'current-content.tex'; pdf=root/'current-design.pdf'
            tex.write_text('old'); pdf.write_bytes(b'pdf')
            record={'inputs': build_inputs(root), 'pdfs': {pdf.name: digest(pdf)}}
            verify_provenance(root, record)
            tex.write_text('new')
            with self.assertRaises(ValueError): verify_provenance(root, record)
            tex.write_text('old'); pdf.write_bytes(b'other')
            with self.assertRaises(ValueError): verify_provenance(root, record)

    def test_target_uses_frozen_signature(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); (root/'api').mkdir()
            shutil.copyfile(Path(__file__).with_name('render-api-contracts.py'), root/'render.py')
            shutil.copyfile(Path(__file__).with_name('render-api-reference.py'), root/'reference.py')
            entry=dict(kind='function', name='X::f', id='API-1', access='public', line=1,
                       signature='void f();', surface='declared-interface')
            inventory={'files':[{'file':'module/X.hpp', 'entries':[entry], 'module':'Core',
                                 'sha256':'fixture', 'parse_errors':[]}]}
            (root/'api/target-inventory.json').write_text(json.dumps(inventory))
            entry['signature']='int f(int changed);'
            (root/'api/inventory.json').write_text(json.dumps(inventory))
            card={'id':'AC-1','title':'test','module':'Core','selectors':[['X.hpp','::f']],
                  'sections':[{'title':'behavior','text':'A & B',
                               'rows':[['cancel','throws_if_terminal'],
                                       ['catalogLookup(objectName)','exact object']],
                               'code':'auto handle = client.request();'}]}
            (root/'target-api-contracts.json').write_text(json.dumps({'cards':[card]}))
            subprocess.run(['python3', str(root/'render.py'), '--target'], check=True, capture_output=True)
            output=(root/'target-api.tex').read_text()
            self.assertIn('void f();',output)
            self.assertLess(output.index('behavior'),output.index('void f();'))
            self.assertNotIn('int f(int changed)',output)
            self.assertIn('所属符号：}X::f',output)
            self.assertIn('A \\& B',output)
            self.assertIn('throws\\_if\\_terminal',output)
            self.assertIn('auto handle = client.request();',output)
            self.assertIn('\\begin{longtable}',output)
            self.assertIn('\\code{catalogLookup(objectName)}',output)
            subprocess.run(['python3', str(root/'reference.py'), '--target'], check=True,
                           capture_output=True)
            reference=(root/'api/target-core-reference.md').read_text()
            self.assertIn('void f();',reference)
            self.assertNotIn('int f(int changed)',reference)
            self.assertIn('冻结源码',reference)
            self.assertNotIn('[源码](../../module/X.hpp',reference)


if __name__ == '__main__':
    unittest.main()
