"""Recreating MiniNDN homes does not rename a node's application cache."""
import ast
import os
import shlex
import xml.etree.ElementTree as ET
from types import SimpleNamespace
import pytest
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


def test_replica_face_uses_the_link_interface_and_rejects_ambiguity():
    tree = ast.parse((ROOT / 'Experiments/NDNSF_RepoOriginalPackets_Minindn.py').read_text())
    main = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                and node.name == 'main')
    face = next(node for node in main.body if isinstance(node, ast.FunctionDef)
                and node.name == 'face')
    ns = {'n': 'ndn:/localhost/nfd/status/1'}
    scope = dict(NS=ns)
    exec(compile(ast.Module(body=[face], type_ignores=[]), '<replica-face>', 'exec'), scope)
    report = ET.fromstring('<nfdStatus xmlns="ndn:/localhost/nfd/status/1"><faces>'
        '<face><faceId>261</faceId><remoteUri>udp4://10.0.0.5:6363</remoteUri></face>'
        '</faces></nfdStatus>')
    peer = SimpleNamespace(name='user', IP=lambda: '10.0.0.1')
    links = [(object(), SimpleNamespace(IP=lambda: '10.0.0.5'))]
    node = SimpleNamespace(connectionsTo=lambda _: links)
    assert scope['face'](report, node, peer).findtext('n:faceId', namespaces=ns) == '261'
    links.append(links[0])
    with pytest.raises(RuntimeError, match='ambiguous physical link'):
        scope['face'](report, node, peer)
    links.pop()
    report.find('n:faces', ns).append(report.find('n:faces/n:face', ns))
    with pytest.raises(RuntimeError, match='ambiguous physical face'):
        scope['face'](report, node, peer)


@pytest.mark.parametrize('changes', [True, False])
def test_replica_counter_snapshot_waits_for_fresh_dataset(changes):
    tree = ast.parse((ROOT / 'Experiments/NDNSF_RepoOriginalPackets_Minindn.py').read_text())
    main = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                and node.name == 'main')
    fresh = next(node for node in main.body if isinstance(node, ast.FunctionDef)
                 and node.name == 'fresh_status')
    ns = {'n': 'ndn:/localhost/nfd/status/1'}
    calls, elapsed = [], [0.0]
    def status(node, label):
        calls.append(label)
        value = 1 if changes and len(calls) >= 3 else 0
        return ET.fromstring('<nfdStatus xmlns="ndn:/localhost/nfd/status/1">'
            '<faces><face><faceId>' + str(value) + '</faceId></face></faces></nfdStatus>')
    def sleep(seconds):
        elapsed[0] += seconds
    scope = dict(NS=ns, ET=ET, status=status, args=SimpleNamespace(timeout_s=0.2),
                 time=SimpleNamespace(monotonic=lambda: elapsed[0], sleep=sleep))
    exec(compile(ast.Module(body=[fresh], type_ignores=[]), '<fresh-status>', 'exec'), scope)
    if changes:
        result = scope['fresh_status'](SimpleNamespace(name='r1'), 'snapshot')
        assert result.findtext('n:faces/n:face/n:faceId', namespaces=ns) == '1'
        assert len(calls) == 3
    else:
        with pytest.raises(RuntimeError, match='fresh Face dataset deadline'):
            scope['fresh_status'](SimpleNamespace(name='r1'), 'snapshot')
    assert len(calls) == len(set(calls))


@pytest.mark.parametrize('changes', [True, False])
def test_replica_route_wait_preserves_exact_target_requirement(changes):
    tree = ast.parse((ROOT / 'Experiments/NDNSF_RepoOriginalPackets_Minindn.py').read_text())
    main = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                and node.name == 'main')
    functions = [node for node in main.body if isinstance(node, ast.FunctionDef)
                 and node.name in ('route_matches', 'wait_route')]
    ns = {'n': 'ndn:/localhost/nfd/status/1'}
    calls, elapsed = [], [0.0]
    def status(node, label):
        calls.append(label)
        value = '266' if changes and len(calls) >= 3 else '264'
        return ET.fromstring('<nfdStatus xmlns="ndn:/localhost/nfd/status/1"><fib>'
            '<fibEntry><prefix>/producer</prefix><nextHops><nextHop><faceId>' + value +
            '</faceId></nextHop></nextHops></fibEntry></fib></nfdStatus>')
    def sleep(seconds):
        elapsed[0] += seconds
    scope = dict(NS=ns, ET=ET, names=['/producer/data'], status=status,
        covers=lambda prefix, name: name.startswith(prefix + '/'),
        Name=SimpleNamespace(from_str=lambda value: value.split('/')),
        args=SimpleNamespace(timeout_s=0.2),
        time=SimpleNamespace(monotonic=lambda: elapsed[0], sleep=sleep))
    exec(compile(ast.Module(body=functions, type_ignores=[]), '<route-wait>', 'exec'), scope)
    if changes:
        scope['wait_route'](SimpleNamespace(name='user'), 'route', '266')
        assert len(calls) == 3
        report = status(None, 'ambiguous')
        entry = report.find('n:fib/n:fibEntry/n:nextHops', ns)
        entry.append(ET.fromstring('<nextHop xmlns="ndn:/localhost/nfd/status/1">'
                                  '<faceId>264</faceId></nextHop>'))
        assert not scope['route_matches'](report, '266')
    else:
        with pytest.raises(RuntimeError, match='exactly one intended Repo'):
            scope['wait_route'](SimpleNamespace(name='user'), 'route', '266')


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
