"""Recreating MiniNDN homes does not rename a node's application cache."""
import ast
import os
import shlex
from types import SimpleNamespace
import __future__
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_replica_children_use_their_node_transport_and_home(tmp_path, monkeypatch):
    tree = ast.parse((ROOT / 'Experiments/NDNSF_RepoOriginalPackets_Minindn.py').read_text())
    main = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                and node.name == 'main')
    launch = next(node for node in main.body if isinstance(node, ast.FunctionDef)
                  and node.name == 'launch')
    captured = []
    def get_popen(node, command, **kwargs):
        captured.append((node.name, kwargs['envDict']))
        return SimpleNamespace(pid=len(captured))
    streams = []
    namespace = dict(os=os, shlex=shlex, output=tmp_path, streams=streams,
                     children=[], owned=[], getPopen=get_popen)
    monkeypatch.setenv('HOME', '/launcher/home')
    monkeypatch.setenv('NDN_CLIENT_TRANSPORT', 'unix:///wrong/socket')
    exec(compile(ast.Module(body=[launch], type_ignores=[]), '<replica-launch>', 'exec'), namespace)
    try:
        for name in ['r1', 'r2', 'user']:
            namespace['launch'](SimpleNamespace(name=name), name, ['oracle'])
        for name, environment in captured:
            assert environment['NDN_CLIENT_TRANSPORT'] == 'unix:///run/nfd/' + name + '.sock'
            assert environment['LD_LIBRARY_PATH'] == '/usr/local/lib'
            assert 'HOME' not in environment
    finally:
        for stream in streams:
            stream.close()


def test_qwen_node_cache_is_independent_of_run_home(tmp_path):
    source = ROOT / 'Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py'
    tree = ast.parse(source.read_text())
    node = next(item for item in tree.body if isinstance(item, ast.FunctionDef)
                and item.name == 'env_for')
    namespace = dict(os=os, Path=Path)
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(source), 'exec',
                 flags=__future__.annotations.compiler_flag), namespace)
    root = tmp_path / 'persistent-cache'
    envs = [namespace['env_for'](tmp_path / run / 'neu', 'neu',
              artifact_cache_root=root,
              requester_publication_root=root / 'ndn/neu/requester-publications')
            for run in ['run-one', 'run-two']]
    assert envs[0]['HOME'] != envs[1]['HOME']
    for key in ['NDNSF_ARTIFACT_CACHE_DIR', 'NDNSF_REQUEST_LARGE_DATA_DIR']:
        assert envs[0][key] == envs[1][key]
        assert str(root) in envs[0][key]


def test_all_qwen_application_environments_receive_persistent_cache():
    source = ROOT / 'Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py'
    tree = ast.parse(source.read_text())
    calls = [node for node in ast.walk(tree) if isinstance(node, ast.Call)
             and isinstance(node.func, ast.Name) and node.func.id == 'env_for']
    assert len(calls) == 4  # Three application launches and one ndnsec setup.
    configured = [call for call in calls
                  if any(keyword.arg == 'artifact_cache_root' for keyword in call.keywords)]
    assert len(configured) == 3
    setup = next(node for node in ast.walk(tree) if isinstance(node, ast.Call)
                 and isinstance(node.func, ast.Name) and node.func.id == 'env_command'
                 and any(isinstance(arg, ast.Constant) and arg.value == 'ndnsec'
                         for arg in node.args))
    assert any(call in list(ast.walk(setup)) for call in calls if call not in configured)


def test_yolo_node_cache_is_independent_of_result_directory():
    source = ROOT / 'Experiments/NDNSF_DI_Yolo2x2_Minindn.py'
    tree = ast.parse(source.read_text())
    start = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                 and node.name == 'start')
    assignment = next(node for node in ast.walk(start) if isinstance(node, ast.Assign)
                      and any(isinstance(target, ast.Subscript)
                              and isinstance(target.value, ast.Name)
                              and target.value.id == 'node_env' for target in node.targets))
    names = {node.id for node in ast.walk(assignment.value) if isinstance(node, ast.Name)}
    assert names == {'cache_root', 'node', 'str'}
