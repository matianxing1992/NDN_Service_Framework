"""Verbatim publication function with fake transport: tests receipt I/O only."""
import ast
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace
import pytest


def test_receipt_written_outside_readonly_config(tmp_path):
    source = Path(__file__).resolve().parents[3] / 'examples/python/NDNSF-DistributedInference/yolo_2x2/controller.py'
    function = next(n for n in ast.parse(source.read_text()).body
                    if isinstance(n, ast.FunctionDef) and n.name == '_publish_spec180_runtime')
    input_dir, output_dir = tmp_path / 'config', tmp_path / 'output'
    input_dir.mkdir()
    output_dir.mkdir()
    publication = input_dir / 'runtime-publication.json'
    publication.write_text('{}')
    input_dir.chmod(0o555)
    class User:
        def __init__(self, **kwargs):
            pass
        def start(self):
            pass
        def publish_signed_app_data(self, name, payload, **kwargs):
            return SimpleNamespace(success=True, data_name=name)
        def fetch_signed_app_data(self, name, signer, **kwargs):
            return SimpleNamespace(success=True, data_name=name, payload=b'catalogue',
                                   signer_certificate='/controller/KEY/k/issuer/v=1')
    deployment = SimpleNamespace(controller='/controller', group='/group', trust_schema='fixture')
    namespace = dict(Path=Path, json=json, hashlib=hashlib, ServiceUser=User,
        APPDeployment=SimpleNamespace(from_config=lambda *a, **kw: SimpleNamespace(deployment=deployment)),
        _decode_spec180_publication=lambda value: ('/controller/catalogue', '/controller', b'catalogue', []))
    exec(compile(ast.Module(body=[function], type_ignores=[]), str(source), 'exec'), namespace)
    try:
        namespace['_publish_spec180_runtime']('fixture', 'fixture', str(publication),
            receipt_path=str(output_dir / 'runtime-publication-receipt.json'))
        receipt = json.loads((output_dir / 'runtime-publication-receipt.json').read_text())
        assert receipt['catalogueDataName'] == '/controller/catalogue'
        assert sorted(p.name for p in input_dir.iterdir()) == ['runtime-publication.json']
        assert publication.read_text() == '{}'
        with pytest.raises(RuntimeError, match='unsafe runtime publication receipt path'):
            namespace['_publish_spec180_runtime']('fixture', 'fixture', str(publication),
                receipt_path=str(output_dir / 'runtime-publication-receipt.json'))
        with pytest.raises(RuntimeError, match='unsafe runtime publication receipt path'):
            namespace['_publish_spec180_runtime']('fixture', 'fixture', str(publication),
                receipt_path=str(publication))
    finally:
        input_dir.chmod(0o755)
