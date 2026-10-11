"""The real YOLO launcher uses the canonical native serving contract."""
import ast
import __future__
import json
from pathlib import Path
import shlex
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'Experiments/NDNSF_DI_Yolo2x2_Minindn.py'


def functions(namespace):
    tree = ast.parse(SOURCE.read_text())
    selected = [node for node in tree.body if isinstance(node, ast.FunctionDef)
                and node.name in ('native_provider_cmd', 'make_native_provider_serving_manifest')]
    exec(compile(ast.Module(body=selected, type_ignores=[]), str(SOURCE), 'exec',
                 flags=__future__.annotations.compiler_flag), namespace)
    return namespace


def test_serving_command_omits_preassembled_references(tmp_path):
    app = '/actual/application'
    namespace = functions(dict(Path=Path, REPO=ROOT, GEN_POLICY=str(tmp_path),
        ARTIFACT_REFERENCES=tmp_path / 'legacy.json', APP_ROOT=app,
        CONTROLLER_IDENTITY=app + '/controller', REPO_SERVICE='/repo/service',
        provider_identity=lambda actor: app + '/provider/' + actor,
        provider_cache_namespace=lambda actor: 'owned-cache',
        perf=SimpleNamespace(shell_quote=lambda value: shlex.quote(str(value)))))
    command = namespace['native_provider_cmd'](
        ['--provider-id', 'A', '--roles', '/Backbone'], '/AI/YOLO', 2, 1, 1,
        Path('/installed/provider'), tmp_path / 'serving.json', tmp_path / 'cache')
    args = shlex.split(command)
    assert '--serve' in args
    assert '--artifact-references' not in args
    assert args[args.index('--manifest') + 1] == str(tmp_path / 'serving.json')
    assert args[args.index('--provider') + 1] == app + '/provider/A'


def test_serving_manifest_requires_canonical_materialization(tmp_path):
    manifest = {'services': [{'name': '/AI/YOLO', 'artifacts': [
        {'role': '/Backbone', 'path': '/preassembled/model.onnx', 'kind': 'model'}]}]}
    (tmp_path / 'service-manifest.json').write_text(json.dumps(manifest))
    namespace = functions(dict(Path=Path, json=json, GEN_POLICY=str(tmp_path)))
    output = namespace['make_native_provider_serving_manifest']('/AI/YOLO')
    artifact = json.loads(output.read_text())['services'][0]['artifacts'][0]
    assert 'path' not in artifact
    assert artifact['materialization'] == 'post-selection-canonical-assembly'
    assert artifact['role'] == '/Backbone'


def test_actual_cold_and_warm_arguments_pass_the_envelope_key(tmp_path):
    tree = ast.parse(SOURCE.read_text())
    warm = next(node for node in ast.walk(tree) if isinstance(node, ast.FunctionDef)
                and node.name == 'build_warm_user_args')
    cold = next(node for node in ast.walk(tree) if isinstance(node, ast.Assign)
                and any(isinstance(target, ast.Name) and target.id == 'user_common'
                        for target in node.targets))
    envelope = tmp_path / 'security/request-envelope.key'
    namespace = dict(common=['--config', 'policy.yaml'], envelope_key_file=envelope,
        ARTIFACT_REFERENCES=tmp_path / 'references.json',
        args_cli=SimpleNamespace(model='yolo.pt', input_size=32, native_providers=False),
        ack_timeout_ms=1500, timeout_ms=90000, user_async_workers=1, native_request_count=4,
        native_assets=None, preflight_requests=0, warm_duration_s=0, warm_interval_ms=0,
        warm_requests=3)
    exec(compile(ast.Module(body=[warm, cold], type_ignores=[]), str(SOURCE), 'exec',
                 flags=__future__.annotations.compiler_flag), namespace)
    for args in (namespace['user_common'], namespace['build_warm_user_args']()):
        assert args.count('--envelope-key-file') == 1
        assert args[args.index('--envelope-key-file') + 1] == str(envelope)
