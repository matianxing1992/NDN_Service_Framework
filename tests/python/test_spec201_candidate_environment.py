"""The installed-candidate CLI selects the model launcher's loader boundary."""
import argparse
import ast
import json
import os
from pathlib import Path
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize('operation', ['freeze', 'check'])
def test_candidate_cli_sets_installed_loader_before_validation(tmp_path, monkeypatch, operation):
    source = ROOT / 'scripts/spec201-native-candidate.py'
    tree = ast.parse(source.read_text())
    main = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                and node.name == 'main')
    blueprint = tmp_path / 'blueprint.json'
    blueprint.write_text(json.dumps(dict(files={}, artifacts=[], nativeReceipts=[],
        prefixRolePlans={}, installedHeaders=[], profileArguments={}, credentialBundles={})))
    candidate = tmp_path / 'candidate.json'
    observed = []

    def freeze(**kwargs):
        observed.append(('freeze', os.environ['LD_LIBRARY_PATH']))
        return {'candidateId': 'fixture'}

    def validate(path, digest, extras):
        observed.append(('check', os.environ['LD_LIBRARY_PATH']))
        assert path == candidate and digest == 'fixture-digest' and extras == []
        return {'candidateId': 'fixture'}

    namespace = dict(argparse=argparse, json=json, os=os, Path=Path,
        freeze_spec201_candidate=freeze, validate_spec201_candidate=validate,
        write_candidate=lambda *args: None, sha256_file=lambda _: 'fixture-digest')
    exec(compile(ast.Module(body=[main], type_ignores=[]), str(source), 'exec'), namespace)
    monkeypatch.setenv('LD_LIBRARY_PATH', str(tmp_path / 'wrong-library-root'))
    monkeypatch.setattr(sys, 'argv', [str(source), operation,
        '--blueprint', str(blueprint), '--candidate', str(candidate),
        '--sha256', 'fixture-digest'])
    namespace['main']()
    assert observed == ([('freeze', '')] if operation == 'freeze' else []) + [
        ('check', '')]
