"""Join runtime observations to manifest actors; never accept ldd as runtime proof."""
import hashlib
import json
import re
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def walk(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from walk(child)
    elif isinstance(value, list):
        for child in value:
            yield from walk(child)


def audit(manifest_path, observation_path, receipt_path, svs_path):
    manifest = json.loads(manifest_path.read_text())
    observation = json.loads(observation_path.read_text())
    receipt = json.loads(receipt_path.read_text())
    assert observation['status'] == 'CAPTURED_PENDING_COVERAGE_AUDIT'
    assert observation['runner_exit_status'] == 0 and not observation['errors']
    assert not observation['signals']
    expected = receipt['artifacts']
    canonical = {str(Path(path).resolve(strict=True)): h for path, h in expected.items()}
    core = {h for p, h in expected.items() if Path(p).name == 'libndn-service-framework.so'}
    assert len(core) == 1
    svs = sha(svs_path)
    processes = list(observation['processes'].values())
    by_pid = {}
    for process in processes:
        by_pid.setdefault(process['pid'], []).append(process)
    actors = {}
    for item in walk(manifest):
        if isinstance(item.get('pid'), int) and 'label' in item and 'host' in item:
            actors.setdefault(item['pid'], []).append(item)
    assert actors, 'no exported process ownership records'
    qualified, auxiliary = [], []
    for pid, owners in actors.items():
        labels = [item['label'] for item in owners]
        imports = []
        for item in owners:
            argv = item.get('argv', [])
            label = item['label']
            declared = (label == 'import-controller-public-certificate-' + item['host'] or
                        re.fullmatch(r'import-controller-target-public-certificate-[0-9]{2}', label))
            if declared:
                assert len(argv) == 5 and argv[1:4] == ['--mode', 'import-public-certificate', '--certificate']
                executable = str(receipt_path.parent / 'spec192-repo-client')
                assert argv[0] == executable and executable in expected
                assert sha(Path(executable)) == expected[executable]
                assert item.get('exit_code') == 0
                log = Path(item['log'])
                assert 'PUBLIC_CERTIFICATE_IMPORTED name=' in log.read_text(errors='replace')
                certificate = Path(argv[4])
                imports.append({'label': label, 'host': item['host'], 'pid': pid,
                    'executable_sha256': expected[executable], 'certificate': str(certificate),
                    'certificate_sha256': sha(certificate), 'log': str(log), 'log_sha256': sha(log),
                    'scope': 'Local PIB setup; returns before constructing Repo API/network actor'})
        if imports:
            assert len(imports) == 1 and all(label == imports[0]['label'] for label in labels)
            auxiliary.extend(imports)
            continue
        arguments = [str(argument) for item in owners for argument in item.get('argv', [])]
        scripts = {Path(argument).name for argument in arguments}
        required_extensions = set()
        if scripts & {'spec192_binding_client.py', 'spec192_di_peer.py', 'spec193_retired_python_peer.py'}:
            required_extensions = {'_ndnsf.', '_py_repoclient.'}
        if any(label.startswith(('spec193-p01-python', 'spec193-p02-python', 'spec193-p03-bindings',
                                 'spec193-p06-retired-python', 'spec192-di-provider-',
                                 'spec192-di-request-', 'python-binding-')) for label in labels):
            required_extensions = {'_ndnsf.', '_py_repoclient.'}
        if 'controller' in labels or 'controller.py' in scripts:
            required_extensions.add('_ndnsf.')
        requires_native = any(item.get('python_maps_observed') or item.get('requires_no_python')
                              for item in owners) or any(argument in expected for argument in arguments)
        if not requires_native and not required_extensions:
            # Certificates, shell probes and NFD are auxiliary, not candidate actors.
            continue
        rows = by_pid.get(pid, [])
        assert len(rows) == 1, 'missing or ambiguous process identity: ' + str(pid)
        row = rows[0]
        maps = list(row['mappings'].values())
        assert maps, 'missing mapped files: ' + str(pid)
        native = any(exe in expected and not exe.endswith('.so') for exe in row['executables'])
        extensions = [m for m in maps if Path(m['path']).name.startswith(('_ndnsf.', '_py_repoclient.'))]
        names = {Path(m['path']).name for m in maps}
        assert not requires_native or native, 'native process executable not captured: ' + repr(labels)
        for prefix in required_extensions:
            assert any(Path(m['path']).name.startswith(prefix) for m in extensions), 'missing required extension: ' + prefix + repr(labels)
        if native or extensions:
            loaded_core = {m['sha256'] for m in maps if Path(m['path']).name.startswith('libndn-service-framework.so')}
            loaded_svs = {m['sha256'] for m in maps if Path(m['path']).name.startswith('libndn-svs.so')}
            assert loaded_core == core, 'missing/foreign actual Core: ' + repr(labels)
            assert loaded_svs == {svs}, 'missing/foreign actual SVS: ' + repr(labels)
            if native:
                assert not any(name.startswith('libpython') for name in names), 'native actor mapped Python'
            if '_ndnsf.' in required_extensions:
                di = {h for p, h in expected.items() if Path(p).name == 'libndnsf-distributed-inference.so'}
                assert {m['sha256'] for m in maps if Path(m['path']).name.startswith('libndnsf-distributed-inference.so')} == di, 'missing/foreign DI library: ' + repr(labels)
            for mapping in maps:
                path = mapping['path']
                if path in canonical:
                    assert mapping['sha256'] == canonical[path], 'foreign candidate artifact: ' + path
                if mapping in extensions:
                    assert path in canonical, 'unreceipted binding extension: ' + path
            qualified.append({'pid': pid, 'birth_ticks': row['birth_ticks'], 'labels': labels,
                              'native': native, 'binding_extensions': [m['path'] for m in extensions]})
    assert qualified, 'no candidate runtime actors qualified'
    return {'status': 'ACTOR_RUNTIME_PASS', 'actors': qualified, 'auxiliary_certificate_imports': auxiliary,
            'manifest_sha256': sha(manifest_path), 'observation_sha256': sha(observation_path),
            'receipt_sha256': sha(receipt_path), 'audit_sha256': sha(Path(__file__))}
