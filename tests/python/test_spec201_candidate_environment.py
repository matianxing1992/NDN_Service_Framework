"""The installed-candidate CLI selects the model launcher's loader boundary."""
import argparse
import ast
import json
import os
from pathlib import Path
import sys

import pytest

from Experiments import NDNSF_DI_Run_Minindn_Regressions as regressions


ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize('operation', ['freeze', 'check'])
@pytest.mark.parametrize('staged', [False, True])
def test_candidate_cli_sets_installed_loader_before_validation(tmp_path, monkeypatch, operation, staged):
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
    command = [str(source), operation,
        '--blueprint', str(blueprint), '--candidate', str(candidate),
        '--sha256', 'fixture-digest']
    expected = ''
    if staged:
        lib = tmp_path / 'lib'
        lib.mkdir()
        expected = str(lib)
        command += ['--native-library-path', expected]
    monkeypatch.setattr(sys, 'argv', command)
    namespace['main']()
    assert observed == ([('freeze', expected)] if operation == 'freeze' else []) + [
        ('check', expected)]


@pytest.mark.parametrize('case_name, default_ack', [('yolo-2x2', 1000), ('yolo-layout', 1500)])
@pytest.mark.parametrize('explicit', [False, True])
def test_regression_forwards_frozen_request_timeouts(monkeypatch, case_name, default_ack, explicit):
    command = [
        'regression', '--case', case_name, '--spec201-candidate', 'candidate.json',
        '--spec201-candidate-sha256', '0' * 64, '--model', 'model.pt',
    ]
    timeouts = {'--ack-timeout-ms': default_ack, '--timeout-ms': 60000}
    if explicit:
        timeouts = {'--ack-timeout-ms': 1200, '--timeout-ms': 2000}
        for option, value in timeouts.items():
            command.extend((option, str(value)))
    observed = []

    def capture(case, args):
        assert case.name == case_name
        observed.append([*case.extra_args, *args])

    monkeypatch.setattr(sys, 'argv', command)
    monkeypatch.setattr(regressions, 'run_case', capture)
    assert regressions.main() == 0
    args, = observed
    for option, value in timeouts.items():
        assert args.count(option) == 1
        assert args[args.index(option) + 1] == str(value)
