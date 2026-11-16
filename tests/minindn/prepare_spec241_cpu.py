#!/usr/bin/env python3
"""Clone explicit native deployment inputs for a bounded CPU qualification."""

import argparse
import base64
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys


def configure_namespace(config, inputs, namespace):
    """Provision separate native keychains; share public certificates only."""
    from ndn.encoding import Name, parse_data

    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'Experiments'))
    from native_probe_validation_links import render_anchored_probe_validation_schema

    config = json.loads(json.dumps(config).replace('/example/repo', namespace))
    for path in inputs.rglob('*'):
        if path.is_file() and path.suffix in ('.json', '.conf', '.info'):
            path.write_text(path.read_text().replace('/example/repo', namespace))
    certificates = {}
    environments = {}
    for actor in config['actors']:
        home = Path(actor['credential_home'])
        try:
            relative = home.relative_to(inputs / 'credentials')
        except ValueError:
            raise ValueError('actor home escapes cloned credentials') from None
        if len(relative.parts) != 1 or home.resolve() != home or home.is_symlink():
            raise ValueError('actor home must be a direct cloned credential directory')
    for actor in config['actors']:
        home = Path(actor['credential_home'])
        shutil.rmtree(home / '.ndn')
        env = dict(os.environ, HOME=str(home))
        for key in ('NDN_CLIENT_PIB', 'NDN_CLIENT_TPM'):
            env.pop(key, None)
        generated = subprocess.run(['ndnsec', 'key-gen', '-t', 'r', actor['identity']],
                                   env=env, capture_output=True, check=True, timeout=30)
        certificate = inputs / 'identity' / (actor['id'] + '.cert')
        certificate.write_bytes(generated.stdout)
        certificates[actor['identity']] = certificate
        environments[actor['identity']] = env
    for identity, env in environments.items():
        subprocess.run(['/usr/local/bin/spec226-public-trust-installer',
                        str(Path(env['HOME']) / '.ndn'), 'tpm-file:',
                        *map(str, certificates.values())], env=env,
                       capture_output=True, check=True, timeout=10)
        subprocess.run(['ndnsec', 'set-default', '-n', identity], env=env,
                       capture_output=True, check=True, timeout=10)
    schema = render_anchored_probe_validation_schema(
        namespace, certificates.values(), sync_prefix=config['network']['group'])
    Path(config['security']['trust_schema']).write_text(schema)
    admission = config['requester']['offer_admission']['policy']
    admission['entries'] = [entry for entry in admission['entries']
                            if entry['provider'] in certificates]
    for entry in admission['entries']:
        wire = base64.b64decode(certificates[entry['provider']].read_bytes())
        name = parse_data(wire)[0]
        entry['certificateName'] = Name.to_str(name)
        entry['keyLocatorPrefix'] = Name.to_str(name[:-2])
    return config


def qualify(config):
    """Declare a single remote stage and conservative operator CPU budgets."""
    import onnx

    model_input = config['requester']['model_input']
    # The installed Requester uses bootstrap_ms as its overall prepareStoredModel budget.
    config['requester']['limits']['bootstrap_ms'] = 600000
    model_input['preparation_state_file'] = str(
        Path(model_input['preparation_state_file']).with_name('preparation-profile-v2.json'))
    selected = config['requester']['request']['provider_names']
    config['actors'] = [actor for actor in config['actors']
                        if actor['kind'] != 'provider' or actor['identity'] in selected]
    repo = next(actor for actor in config['actors'] if actor['kind'] == 'repo')
    repo.setdefault('parameters', {})['request-timeout-ms'] = 30000
    catalog = config['requester']['catalog']
    split = catalog['splitter']
    if len(split['roles']) != 1:
        raise ValueError('this qualification requires one complete remote model stage')
    graph = onnx.load_model(catalog['source']['file'], load_external_data=False).graph
    weight_bytes = sum(math.prod(tensor.dims) *
                       onnx.helper.tensor_dtype_to_np_dtype(tensor.data_type).itemsize
                       for tensor in graph.initializer)
    split['resources_by_role'] = {
        role: {'backends': catalog['model']['adapter']['backends'],
               'weight_bytes': max(split['weight_bytes_by_role'][role], weight_bytes),
               'workspace_bytes': 1 << 30, 'kv_bytes': 64 << 20,
               'activation_bytes': 64 << 20, 'transient_bytes': 2 << 30,
               'safety_margin': 1.25} for role in split['roles']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--namespace', help='new explicit deployment root and fresh native credentials')
    args = parser.parse_args()
    baseline = args.baseline.resolve()
    source = baseline.parent.parent
    output = args.output.resolve()
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    inputs = output / 'inputs'
    inputs.mkdir(mode=0o700)
    for name in ('credentials', 'authority', 'requester', 'provider-0', 'provider-1', 'identity'):
        shutil.copytree(source / name, inputs / name)
    for name in ('trust-schema.conf', 'controller-policy.info',
                 'native-qwen-execution-plan.json', 'native-qwen-service-manifest.json'):
        shutil.copy2(source / name, inputs / name)
    for path in inputs.rglob('*'):
        if path.is_file() and path.suffix in ('.json', '.conf', '.info'):
            path.write_text(path.read_text().replace(str(source), str(inputs)))
    config = json.loads(baseline.read_text().replace(str(source), str(inputs)))
    config['deployment_id'] = 'spec241-cpu-cold-warm'
    config['release']['manifest'] = str(output / 'installed.json')
    for node in config['nodes']:
        node['nfd']['socket'] = str(output / 'sockets' / (node['id'] + '.sock'))
    for key in config['storage']:
        if key != 'max_bytes':
            config['storage'][key] = str(output / key)
    config['storage']['scratch_root'] = '/tmp/ndnsf-spec241-cpu-scratch'
    requester = config['requester']
    for key in ('encrypted_repository', 'repository', 'kv_handoff', 'test_stream_barrier',
                'test_handoff_barrier'):
        requester.pop(key, None)
    repo = next(actor for actor in config['actors'] if actor['kind'] == 'repo')
    requester['model_input'] = {
        'mode': 'source',
        'repo_target': {'repo_node': repo['identity'], 'provider_identity': repo['identity'],
                        'service_prefix': config['network']['repo_service']},
        'preparation_state_file': str(output / 'private-model' / 'preparation.json'),
        'private_handle_file': str(output / 'private-model' / 'handle.json'),
    }
    requester['conversation']['journal']['state_root'] = str(output / 'journal')
    for index, turn in enumerate(requester['turns']):
        turn['output_file'] = str(output / 'evidence_root' / ('output-' + str(index) + '.json'))
        turn['checkpoint_output_file'] = str(output / 'evidence_root' /
                                            ('checkpoint-' + str(index) + '.json'))
    for actor in config['actors']:
        if actor['kind'] == 'provider':
            actor['parameters'].pop('kv-handoff-config', None)
    qualify(config)
    if args.namespace:
        config = configure_namespace(config, inputs, args.namespace)
    ledger = {'schema': 1, 'files': {}}
    prefix = Path(config['release']['prefix'])
    paths = [prefix / 'bin' / name for name in (
        'App_ServiceController', 'DI_NativeArtifactAuthority', 'DI_NativeRequester',
        'di-native-provider', 'DistributedRepoServer', 'ndnsf-identity-check')]
    paths.append(prefix / 'libexec/ndnsf-di/DI_NativeOnnxAssemblyWorker')
    paths.extend(prefix / 'lib' / name for name in (
        'libndn-service-framework.so', 'libndnsf-distributed-inference.so'))
    for path in paths:
        if path.is_symlink():
            ledger['files'][str(path)] = {'installed': {
                'kind': 'symlink', 'target': os.readlink(path)}}
            path = path.resolve()
        ledger['files'][str(path)] = {'installed': {
            'kind': 'file', 'mode': stat.S_IMODE(path.stat().st_mode),
            'sha256': 'sha256:' + hashlib.sha256(path.read_bytes()).hexdigest()}}
    (output / 'installed.json').write_text(json.dumps(ledger, indent=2) + '\n')
    path = output / 'deployment.json'
    path.write_text(json.dumps(config, indent=2) + '\n')
    print(path)


if __name__ == '__main__':
    main()
