"""Stable experiment package storage preserves bytes and protection identity."""
import ast
import __future__
import fcntl
import hashlib
import json
import os
from pathlib import Path
from types import SimpleNamespace

import pytest


SOURCE = Path(__file__).resolve().parents[2] / 'Experiments/native_yolo_requester.py'


def helpers():
    names = {'_canonical_bytes', '_digest_bytes', '_digest_value', '_write_json',
             '_write_ed25519_keypair', '_public_key_id', '_cached_yolo_package'}
    nodes = [node for node in ast.parse(SOURCE.read_text()).body
             if isinstance(node, ast.FunctionDef) and node.name in names]
    namespace = dict(Path=Path, os=os, fcntl=fcntl, json=json, hashlib=hashlib)
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(SOURCE), 'exec',
                 flags=__future__.annotations.compiler_flag), namespace)
    return namespace['_cached_yolo_package']


def exporter(calls):
    def build(checkpoint, package, **kwargs):
        calls.append(str(package))
        package.mkdir(parents=True, exist_ok=True)
        manifest = {'source': {'checkpointSha256': hashlib.sha256(checkpoint.read_bytes()).hexdigest()}}
        (package / 'manifest.json').write_text(json.dumps(manifest))
        (package / 'model.onnx').write_bytes(b'fixed-model')
        return manifest
    return SimpleNamespace(build_package=build)


def test_same_inputs_reuse_one_package_and_content_key(tmp_path):
    checkpoint = tmp_path / 'model.pt'
    checkpoint.write_bytes(b'checkpoint')
    calls = []
    cache = helpers()
    args = (tmp_path / 'cache', {'inputSize': 32, 'controller': '/producer'},
            exporter(calls), checkpoint, 32, '/producer')
    first = cache(*args)
    before = (first[0] / 'manifest.json').stat().st_mtime_ns
    second = cache(*args)
    assert len(calls) == 1
    assert first == second
    assert (second[0] / 'manifest.json').stat().st_mtime_ns == before
    assert len(first[3]) == 32
    assert len(list((tmp_path / 'cache').iterdir())) == 1
    assert not list((tmp_path / 'cache').rglob('*private.pem'))


def test_changed_export_identity_has_separate_protection(tmp_path):
    checkpoint = tmp_path / 'model.pt'
    checkpoint.write_bytes(b'checkpoint')
    calls, cache = [], helpers()
    first = cache(tmp_path / 'cache', {'inputSize': 32}, exporter(calls), checkpoint, 32, '/p')
    second = cache(tmp_path / 'cache', {'inputSize': 64}, exporter(calls), checkpoint, 64, '/p')
    assert len(calls) == 2
    assert first[0] != second[0]
    assert first[3] != second[3]


@pytest.mark.parametrize('mutation', ['permissions', 'symlink', 'marker'])
def test_cache_rejects_changed_key_or_identity(tmp_path, mutation):
    checkpoint = tmp_path / 'model.pt'
    checkpoint.write_bytes(b'checkpoint')
    cache = helpers()
    args = (tmp_path / 'cache', {'inputSize': 32}, exporter([]), checkpoint, 32, '/p')
    package, _, _, _ = cache(*args)
    directory = package.parent
    key = directory / 'content.key'
    if mutation == 'permissions':
        key.chmod(0o644)
    elif mutation == 'symlink':
        target = tmp_path / 'outside.key'
        target.write_bytes(bytes(32))
        key.unlink()
        key.symlink_to(target)
    else:
        (directory / 'cache-identity.json').write_text('{}')
    with pytest.raises(RuntimeError, match='YOLO_PACKAGE_'):
        cache(*args)


def test_runtime_inputs_include_shared_cached_package():
    tree = ast.parse(SOURCE.read_text())
    returns = [node for node in ast.walk(tree) if isinstance(node, ast.Dict)
               and any(isinstance(key, ast.Constant) and key.value == 'runtime_inputs'
                       for key in node.keys)]
    assert len(returns) == 1
    value = next(value for key, value in zip(returns[0].keys, returns[0].values)
                 if isinstance(key, ast.Constant) and key.value == 'runtime_inputs')
    names = {node.id for node in ast.walk(value) if isinstance(node, ast.Name)}
    assert {'package_dir', 'catalogue_registry', 'bundle_root'} <= names


def test_export_identity_pins_fixture_and_numpy():
    tree = ast.parse(SOURCE.read_text())
    identity = next(node.value for node in ast.walk(tree) if isinstance(node, ast.Assign)
                    and any(isinstance(target, ast.Name) and target.id == 'identity'
                            for target in node.targets))
    values = {key.value: value for key, value in zip(identity.keys, identity.values)}
    assert {'fixtureDigest', 'numpyVersion'} <= set(values)
    assert any(isinstance(node, ast.Name) and node.id == 'fixture_path'
               for node in ast.walk(values['fixtureDigest']))
    call = next(node for node in ast.walk(tree) if isinstance(node, ast.Call)
                and isinstance(node.func, ast.Name) and node.func.id == '_cached_yolo_package')
    assert any(keyword.arg == 'fixture_path' and isinstance(keyword.value, ast.Name)
               and keyword.value.id == 'fixture_path' for keyword in call.keywords)


def test_requester_source_files_point_to_actual_shared_package(tmp_path):
    tree = ast.parse(SOURCE.read_text())
    binding = next(node for node in ast.walk(tree) if isinstance(node, ast.Dict)
                   and any(isinstance(key, ast.Constant) and key.value == 'initializer_file'
                           for key in node.keys))
    fields = {key.value: value for key, value in zip(binding.keys, binding.values)
              if isinstance(key, ast.Constant)}
    graph_path = tmp_path / 'cache/canonical-package/canonical/yolo26n.onnx'
    weights_path = graph_path.with_suffix('.weights')
    namespace = dict(graph_path=graph_path, weights_path=weights_path)
    for name, expected in [('file', graph_path), ('initializer_file', weights_path)]:
        result = eval(compile(ast.Expression(fields[name]), str(SOURCE), 'eval'), namespace)
        assert result == str(expected.resolve())
        assert Path(result).is_absolute()
