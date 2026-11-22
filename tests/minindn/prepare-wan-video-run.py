#!/usr/bin/env python3
"""Prepare a real installed MiniNDN deployment of the C++ Wan video fixture."""

import argparse
import copy
import json
import os
from pathlib import Path
import runpy
import shutil
import stat
import subprocess
import sys

CHECKOUT = Path(__file__).resolve().parents[2]
TOPOLOGY = CHECKOUT / 'Experiments/Topology/spec261-hybrid.conf'
HOSTS = ('requester', 'control', 'provider', 'provider2')
PROVIDERS = ('provider', 'provider2', 'control')
COMPONENTS = ('umt5', 'dit', 'vae')


def installed_ledger(prefix, file_digest, capture_fixture_trace):
    files = {}
    paths = [prefix / 'bin' / program for program in (
        'App_ServiceController', 'DI_NativeArtifactAuthority', 'DI_NativeRequester',
        'di-native-provider', 'DistributedRepoServer', 'ndnsf-identity-check')]
    if capture_fixture_trace:
        paths.append(prefix / 'bin/di-wan-video-oracle')
    paths += [prefix / 'libexec/ndnsf-di/DI_NativeOnnxAssemblyWorker']
    paths += [prefix / 'lib' / name for name in (
        'libndn-service-framework.so', 'libndnsf-distributed-inference.so')]
    for path in paths:
        if path.is_symlink():
            files[str(path)] = {'installed': {'kind': 'symlink', 'target': os.readlink(path)}}
            path = path.resolve(strict=True)
            path.relative_to(prefix)
        files[str(path)] = {'installed': dict(
            kind='file', mode=stat.S_IMODE(path.stat().st_mode), sha256=file_digest(path))}
    return dict(schema=1, files=files)


def prepare(root, prefix, builder, mode, namespace, inline_limit, exact_packet_disk_mib,
            capture_fixture_trace, tile_dit=False, tensor_segment_bytes=None,
            checkpoint_workload=None):
    sys.path.insert(0, str(CHECKOUT / 'Experiments'))
    deployment = runpy.run_path(str(CHECKOUT / 'Experiments/prepare-native-deployment.py'))
    credentials = runpy.run_path(str(CHECKOUT / 'tests/fixtures/spec242/prepare-deployment.py'))
    digest, write_json = deployment['digest'], deployment['write_json']
    ledger = installed_ledger(prefix, deployment['file_digest'], capture_fixture_trace)
    root.mkdir(mode=0o700)
    network = dict(group=namespace + '/group', service=namespace + '/infer',
                   artifact_service=namespace + '/grants', repo_service=namespace + '/repo-objects')
    requester = namespace + '/requester'
    authority = namespace + '/authority'
    model_family = '/Model/Test/Wan271'
    content_key_id = 'wan-fixture-content-v1'
    epoch = 'epoch-1'
    workload = root / 'workload'
    if checkpoint_workload is None:
        build_command = [str(builder), str(workload), requester, network['service'], epoch]
        if tile_dit:
            build_command.append('--tile-dit')
        subprocess.run(build_command, check=True, timeout=120)
        fixture = json.loads((workload / 'fixture.json').read_bytes())
    else:
        if tile_dit:
            raise ValueError('--tile-dit applies only to the generated tiny fixture')
        fixture = json.loads(checkpoint_workload.read_bytes())
        if (fixture.get('schema') != 'ndnsf-di-wan-checkpoint-workload-v1' or
                fixture['namespace'] != namespace or fixture['mode'] != mode):
            raise ValueError('checkpoint workload namespace or layout differs')
        workload.mkdir(mode=0o700)
        write_json(workload / 'fixture.json', fixture)
    layout = fixture['modes'][mode]
    count = max(sum(map(len, row['stages'])) for row in layout.values())
    providers = ['provider-' + str(rank) for rank in range(count)]
    by_role = {role: providers[rank] for component in COMPONENTS
               for rank, role in enumerate(sum(layout[component]['stages'], []))}
    provider_roles = {provider: [role for role, actor in by_role.items() if actor == provider]
                      for provider in providers}

    topology = root / 'topology.conf'
    shutil.copyfile(TOPOLOGY, topology)
    actor_rows = [('controller', 'controller', 'requester'),
                  ('authority', 'artifact-authority', 'requester'),
                  ('repo', 'repo', 'requester'), ('requester', 'requester', 'requester')]
    actor_rows += [(provider, 'provider', host)
                   for provider, host in zip(providers, PROVIDERS)]
    identities = {actor: namespace + '/' + actor for actor, _, _ in actor_rows}
    actors = [dict(id=actor, kind=kind, node=node, identity=identities[actor],
                   resources=dict(cpus=1, allow_sharing=True), parameters={})
              for actor, kind, node in actor_rows]
    if checkpoint_workload is not None:
        next(actor for actor in actors if actor['kind'] == 'repo')['parameters'].update({
            'request-timeout-ms': 60000,
        })
    environments, certificates, trust = credentials['prepare_identities'](
        root, prefix, namespace, network['group'], actors)
    policy = root / 'controller-policy.info'
    policy.write_text(deployment['policy_text'](identities, network, provider_roles, None))

    key_root = root / 'artifact-keys'
    key_root.mkdir(mode=0o700)
    openssl = shutil.which('openssl')
    if not openssl:
        raise RuntimeError('openssl is required by the existing artifact key generator')
    key = deployment['ed25519_key']
    issuer_private, issuer_public, _ = key(openssl, key_root / 'issuer', environments['authority'], root)
    user_private, user_public, _ = key(openssl, key_root / 'requester', environments['requester'], root)
    content = key_root / 'content.bin'
    credentials['secure_random'](content)
    recipients, offer_keys, offer_entries = {}, {}, []
    for actor in actors:
        name, identity = actor['id'], actor['identity']
        if name not in provider_roles:
            continue
        env = environments[name]
        private, public, _ = key(openssl, key_root / (name + '-recipient'), env, root)
        offer, offer_public, signer = key(openssl, key_root / (name + '-offer'), env, root)
        recipients[identity], offer_keys[signer] = str(public), str(offer_public)
        mapping = key_root / (name + '-recipient.json')
        write_json(mapping, {identity: str(private)})
        public_dir = Path(actor['credential_home']) / 'public'
        public_dir.mkdir(mode=0o700)
        hint = public_dir / 'authority-public.pem'
        shutil.copyfile(issuer_public, hint)
        write_json(public_dir / 'trust-root-registry-v1.json', {
            'schemaVersion': 1, 'status': 'CONFIGURED',
            'artifactPolicyAuthority': dict(
                acceptedModelFamilies=[model_family], authorityId=authority, keyId=content_key_id,
                protectionEpochs=[epoch], publicKeyAlgorithm='ed25519',
                publicKeyPath='public/authority-public.pem', publicKeySha256=digest(hint.read_bytes()),
                signatureAlgorithm='ed25519', grantSchema='ndnsf-di-key-grant-v1'),
        })
        metadata = json.loads(deployment['run'](
            [prefix / 'bin/ndnsf-identity-check', '--credential-metadata',
             actor['credential_home'], identity], env, root))
        offer_entries.append(dict(provider=identity, service=network['service'], signerKeyId=signer,
                                  keyLocatorPrefix=metadata['keyName'],
                                  certificateName=metadata['certificateName']))
        actor['parameters'].update({
            'roles': ','.join(provider_roles[name]), 'grant-authority-public-key': str(hint),
            'recipient-key-map': str(mapping), 'selection-offer-key-file': str(offer),
            'offer-backend': 'onnxruntime-cpu', 'offer-can-provision': True,
            'assembly-timeout-ms': 120000, 'permission-wait-ms': 30000,
            'repo-fetch-timeout-ms': 60000,
        })
        if exact_packet_disk_mib:
            actor['parameters']['exact-packet-disk-mib'] = exact_packet_disk_mib
        if checkpoint_workload is not None:
            actor['parameters'].update({
                'assembly-timeout-ms': 3600000, 'repo-fetch-timeout-ms': 3600000,
                # Admission reserves source + assembled ceilings for all three components.
                'artifact-cache-max-mib': 393216,
            })
        if tensor_segment_bytes is not None:
            actor['parameters']['tensor-segment-bytes'] = tensor_segment_bytes

    service_fields = dict(model=model_family, modelFamily='wan', modelFormat='onnx',
                          modelRepository='wan-test-fixture', modelRevision='1', dtype='fp32',
                          quantization='none', runtimeBackend='onnxruntime',
                          plannerKind='native-wan-canonical', roles=list(by_role), dependencies=[])
    plan, manifest = root / 'plan.json', root / 'service-manifest.json'
    write_json(plan, dict(version=2, services=[
        dict(service_fields, service=network['service'], executionPolicy='DATA_DRIVEN_V2',
             schemaVersion=3)]))
    write_json(manifest, dict(version=2, services=[
        dict(service_fields, name=network['service'], artifacts=[])]))
    tokenizer = root / 'tokenizer.json'
    write_json(tokenizer, {'version': '1.0', 'model': {'type': 'WordLevel',
        'vocab': {'<pad>': 0, '</s>': 1, 'clip': 2}, 'unk_token': '<pad>'}})

    catalogs, publication_sources = {}, {}
    for component in COMPONENTS:
        path = workload / layout[component]['catalog']
        catalog = json.loads(path.read_bytes())
        catalogs[component] = (catalog, path.parent)
        model, source = catalog['model'], catalog['source']
        publication_sources[source['model_manifest_digest']] = dict(
            model_name=model['model_name'], model_content_digest=model['content_digest'],
            canonical_source_digest=source['digest'],
            initializer_object_digest=source.get('initializer_digest', ''),
            artifact_profile_digest=catalog['recipe']['artifact_profile_digest'])

    repo_target = dict(repo_node=identities['repo'], provider_identity=identities['repo'],
                       service_prefix=network['repo_service'])
    grant = dict(authority_service=network['artifact_service'], authority_identity=authority,
                 requester_private_key_file=str(user_private),
                 authority_public_key_file=str(issuer_public), protection_epoch=epoch)
    core = dict(requester_identity=requester, authority_identity=identities['controller'],
                group=network['group'], trust_schema_file=str(trust))
    identity = digest(b'ndnsf-wan-fixture/request-v1')
    request = dict(schema='ndnsf-di-native-request-runtime-v1', service=network['service'],
                   task='task', provider_names=[identities[role] for role in providers],
                   adapter_composition_digest=identity, task_descriptor_digest=identity,
                   input_layout_digest=identity, security_policy_digest=digest(policy.read_bytes()),
                   timeout_ms=180000, ack_timeout_ms=2000, max_candidates=1, max_policy_ms=5000)

    def requester_config(component):
        catalog, base = catalogs[component]
        catalog = copy.deepcopy(catalog)
        if component == 'umt5' or checkpoint_workload is not None:
            for name in ('file', 'initializer_file'):
                if name in catalog['source']:
                    catalog['source'][name] = str(base / catalog['source'][name])
        candidate = layout[component]['candidate_digest']
        return dict(schema='ndnsf-di-native-requester-v1', core=core, grant=grant, request=request,
                    catalog=catalog, model_input=dict(
                        mode='source', repo_target=repo_target,
                        preparation_state_file=str(root / 'models' / component / 'preparation.json'),
                        private_handle_file=str(root / 'models' / component / 'handle.json')),
                    limits=dict(bootstrap_ms=7200000 if checkpoint_workload else 120000,
                                max_prepared_entries=6,
                                max_prepared_bytes=512 << 30 if checkpoint_workload else 512 << 20,
                                max_assembled_bytes=catalog['recipe']['max_assembled_bytes'],
                                max_source_bytes=catalog['recipe']['max_source_bytes']),
                    offer_admission=dict(candidate_digest=candidate, public_key_files=offer_keys,
                        policy=dict(schema='spec180-provider-offer-trust-v1',
                                    candidateDigest=candidate, candidateId=candidate[7:],
                                    trustSchema=namespace + '/trust', entries=offer_entries)))

    wan = copy.deepcopy(fixture['generation']) if checkpoint_workload else {
        name: fixture[name] for name in (
            'latent_shape', 'padded_video_tokens', 'text_width', 'steps', 'shift',
            'guidance', 'seed', 'positive_tokens', 'negative_tokens')}
    wan.update(output_directory=str(root / 'evidence' / 'clip'), generation_id='wan-video-' + mode,
               timeout_ms=86400000 if checkpoint_workload else 900000,
               inline_limit_bytes=inline_limit)
    if checkpoint_workload:
        request['timeout_ms'] = 3600000
        if mode != 'single':
            # A real VAE edge can exceed the default 4096 segments.
            request['max_segments'] = 16384
    for component, config_name in (('dit', 'denoiser_config_file'), ('vae', 'decoder_config_file')):
        base = workload / component if checkpoint_workload else catalogs[component][1]
        base.mkdir(mode=0o700, exist_ok=True)
        path = base / 'requester.json'
        write_json(path, requester_config(component))
        wan[config_name] = str(path)
    main = requester_config('umt5')
    main.update(wan_generation=wan, fixed_placement={
        'role_providers': {role: identities[provider] for role, provider in by_role.items()}})
    if capture_fixture_trace:
        main.update(test_only=True, test_wan_fixture_trace=True)
        if checkpoint_workload:
            main['test_wan_fixture_trace_max_bytes'] = 2 << 30
    artifact_authority = dict(authority=dict(
        authority_private_key_file=str(issuer_private), content_key_file=str(content),
        content_key_id=content_key_id, protection_epoch=epoch,
        requester_public_key_files={requester: str(user_public)},
        recipient_public_key_files=recipients, allowed_model_manifests=sorted(publication_sources),
        publication_sources=publication_sources),
        max_grant_ttl_ms=3600000 if checkpoint_workload else 900000)
    ledger_path = root / 'installed.json'
    write_json(ledger_path, ledger)
    path = root / 'deployment.json'
    write_json(path, dict(
        schema='ndnsf-di-deployment-v1', deployment_id='wan-video-' + mode,
        release=dict(prefix=str(prefix), manifest=str(ledger_path)),
        platform=dict(kind='minindn', topology=str(topology),
                      topology_sha256=digest(topology.read_bytes())),
        nodes=[dict(id=node, host=node, cpus=8, gpu_devices=[], nfd=dict(
            mode='managed', socket=str(root / 'sockets' / (node + '.sock')),
            address='127.0.0.1', port=6363)) for node in HOSTS],
        actors=actors, network=network,
        security=dict(trust_schema=str(trust), controller_policy=str(policy),
                      controller_certificate=certificates['controller']),
        storage={**{key + '_root': str(root / key) for key in
                    ('cache', 'evidence', 'repo', 'scratch', 'state')},
                 'max_bytes': 192 << 30 if checkpoint_workload else 512 << 20},
        model=dict(plan=str(plan), manifest=str(manifest), tokenizer=str(tokenizer)),
        requester=main, artifact_authority=artifact_authority))
    return path


def prepare_warm(cold_path):
    root = cold_path.parent
    cold = json.loads(cold_path.read_bytes())
    main = cold['requester']
    generation = main['wan_generation']
    if not (root / 'workload/fixture.json').is_file():
        raise ValueError('warm preparation requires a generated Wan fixture')
    if not (Path(generation['output_directory']) / 'manifest.json').is_file():
        raise ValueError('warm preparation requires a completed cold clip')

    def bind_handle(config):
        model = config['model_input']
        handle = Path(model['private_handle_file'])
        handle.resolve().relative_to(root / 'models')
        if model['mode'] != 'source' or not handle.is_file():
            raise ValueError('warm preparation requires a cold private model handle')
        model['mode'] = 'stored'
        del model['preparation_state_file']
        source = config['catalog']['source']
        for field in ('file', 'initializer_file'):
            source.pop(field, None)

    bind_handle(main)
    files = {}
    for component in ('denoiser', 'decoder'):
        field = component + '_config_file'
        path = Path(generation[field]).resolve()
        path.relative_to(root / 'workload')
        secondary = json.loads(path.read_bytes())
        bind_handle(secondary)
        path = path.with_name('requester-warm.json')
        files[path] = secondary
        generation[field] = str(path)
    generation['output_directory'] = str(root / 'evidence/clip-warm')
    output = root / 'deployment-warm.json'
    files[output] = cold
    if any(path.exists() for path in files):
        raise FileExistsError('warm fixture configuration already exists')
    created = []
    try:
        for path, value in files.items():
            with path.open('x', encoding='utf-8') as stream:
                created.append(path)
                json.dump(value, stream, sort_keys=True, separators=(',', ':'))
                stream.write('\n')
    except Exception:
        for path in created:
            path.unlink()
        raise
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--warm-from', type=Path, help='cold fixture deployment.json')
    parser.add_argument('--prefix', type=Path)
    parser.add_argument('--builder', type=Path)
    parser.add_argument('--checkpoint-workload', type=Path,
                        help='explicit real-checkpoint experiment manifest; no tiny builder')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--namespace', default='/ndnsf-test/wan')
    parser.add_argument('--mode', choices=('single', 'tp', 'pp', 'pp-tp', 'tp-pp'))
    parser.add_argument('--inline-limit-bytes', type=int, default=4096)
    parser.add_argument('--exact-packet-disk-mib', type=int, default=0)
    parser.add_argument('--tensor-segment-bytes', type=int)
    parser.add_argument('--capture-fixture-trace', action='store_true')
    parser.add_argument('--tile-dit', action='store_true')
    args = parser.parse_args()
    if args.inline_limit_bytes <= 0:
        parser.error('--inline-limit-bytes must be positive')
    if args.exact_packet_disk_mib < 0:
        parser.error('--exact-packet-disk-mib must be nonnegative')
    if args.tensor_segment_bytes is not None and args.tensor_segment_bytes <= 0:
        parser.error('--tensor-segment-bytes must be positive')
    os.umask(0o077)
    if args.warm_from:
        if any((args.prefix, args.builder, args.output, args.mode, args.exact_packet_disk_mib,
                args.capture_fixture_trace, args.tile_dit, args.tensor_segment_bytes,
                args.checkpoint_workload)):
            parser.error('--warm-from uses the cold deployment paths')
        print(prepare_warm(args.warm_from.resolve()))
    else:
        if (not all((args.prefix, args.output, args.mode)) or
                bool(args.builder) == bool(args.checkpoint_workload)):
            parser.error('cold preparation requires prefix/output/mode and one builder or workload')
        print(prepare(args.output.resolve(), args.prefix.resolve(),
                      args.builder.resolve() if args.builder else None,
                      args.mode, args.namespace, args.inline_limit_bytes,
                      args.exact_packet_disk_mib, args.capture_fixture_trace, args.tile_dit,
                      args.tensor_segment_bytes,
                      args.checkpoint_workload.resolve() if args.checkpoint_workload else None))


if __name__ == '__main__':
    main()
