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
            entry=dict(kind='function', name='X::f', id='API-1', access='public', line=1, signature='void f();')
            inventory={'files':[{'file':'module/X.hpp', 'entries':[entry]}]}
            (root/'api/target-inventory.json').write_text(json.dumps(inventory))
            entry['signature']='int f(int changed);'
            (root/'api/inventory.json').write_text(json.dumps(inventory))
            card={'id':'AC-1','title':'test','module':'Core','selectors':[['X.hpp','::f']], 'sections':[]}
            (root/'target-api-contracts.json').write_text(json.dumps({'cards':[card]}))
            subprocess.run(['python3', str(root/'render.py'), '--target'], check=True, capture_output=True)
            output=(root/'target-api.tex').read_text()
            self.assertIn('void f();',output)
            self.assertNotIn('int f(int changed)',output)


if __name__ == '__main__':
    unittest.main()
