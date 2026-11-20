#!/usr/bin/env python3
"""Derive one network cell from a prepared layout deployment root.

The layout root keeps the Provider identities, permissions, publication
handles, Repo source and the assembled Provider slices (E6.1). A cell only
replaces the network inputs: its Provider data-link topology parameters and
the sidecar record that names the cell. Everything else stays byte-identical,
so the Provider cache keys and the prepared state remain valid.
"""

import argparse
import hashlib
import json
import re
from itertools import combinations
from pathlib import Path

CHECKOUT = Path(__file__).resolve().parents[1]
HISTORICAL_STAR = CHECKOUT / 'tests/fixtures/spec242/minindn-topology.conf'

NETWORKS = {
    'N0': (100, 2, 0),
    'N1': (10, 2, 0),
    'N2': (1000, 2, 0),
    'N3': (100, 10, 0),
    'N4': (100, 30, 0),
    'N5': (100, 2, 0.1),
    'N6': (100, 2, 1),
    'N7': (10, 10, 0.1),
    'N8': (10, 30, 1),
}
HISTORICAL_BANDWIDTHS = {'N0': None, 'N3': 1000, 'N4': 1000, 'N5': 1000, 'N6': 1000}


def _derive_star(base, providers, bandwidth, delay, loss):
    lines = []
    for line in base.splitlines():
        stripped = line.strip()
        shaped = (':' in stripped and 'delay=' in stripped and
                  any(node in stripped.split()[0].split(':') for node in providers))
        if shaped and (bandwidth is not None or delay is not None or loss is not None):
            if bandwidth is not None:
                line = re.sub(r'bw=[0-9.]+', f'bw={bandwidth:g}', line)
            if delay is not None:
                line = re.sub(r'delay=[0-9.]+ms', f'delay={delay:g}ms', line)
            if loss is not None:
                if re.search(r'loss=[0-9.]+', line):
                    line = re.sub(r'loss=[0-9.]+', f'loss={loss:g}', line)
                else:
                    line = re.sub(r'(delay=[0-9.]+ms)', r'\1 loss=' + f'{loss:g}', line)
        lines.append(line)
    return '\n'.join(lines) + '\n'


def _derive_star_base(base, deployment, bandwidth, delay, loss, shaped_nodes):
    """Shape only the Relay-Provider access links of an explicit star base."""
    hosts = {node['host'] for node in deployment['nodes']}
    relay_hosts = [host for host in hosts if host.startswith('h242relay')]
    if len(relay_hosts) != 1:
        raise ValueError('star base topology needs exactly one Relay host')
    relay = relay_hosts[0]
    lines = []
    for line in base.splitlines():
        stripped = line.strip()
        if ':' in stripped and 'delay=' in stripped:
            pair = stripped.split()[0].split(':')
            access = ((pair[0] == relay and pair[1] in shaped_nodes) or
                      (pair[1] == relay and pair[0] in shaped_nodes))
            if access:
                line = re.sub(r'bw=[0-9.]+', f'bw={bandwidth:g}', line)
                line = re.sub(r'delay=[0-9.]+ms', f'delay={delay:g}ms', line)
                if loss:
                    if re.search(r'loss=[0-9.]+', line):
                        line = re.sub(r'loss=[0-9.]+', f'loss={loss:g}', line)
                    else:
                        line = re.sub(r'(delay=[0-9.]+ms)', r'\1 loss=' + f'{loss:g}', line)
        lines.append(line)
    return '\n'.join(lines) + '\n', relay


def _derive_mesh(base, deployment, provider_nodes, bandwidth, delay, loss):
    hosts_by_id = {node['id']: node['host'] for node in deployment['nodes']}
    hosts = set(hosts_by_id.values())
    if len(deployment['nodes']) != 8 or len(hosts_by_id) != 8 or len(hosts) != 8:
        raise ValueError('K8 requires exactly eight distinct deployment nodes and hosts')

    providers = {actor['node'] for actor in deployment['actors'] if actor['kind'] == 'provider'}
    support = {actor['node'] for actor in deployment['actors'] if actor['kind'] != 'provider'}
    pool = set(provider_nodes)
    if (len(provider_nodes) != 4 or len(pool) != 4 or not pool <= hosts_by_id.keys() or
            not providers <= pool or pool & support):
        raise ValueError('K8 needs four distinct --provider-node IDs covering the Provider pool')

    expected_links = set(combinations(sorted(hosts), 2))
    shaped_links = set(combinations(sorted(hosts_by_id[node] for node in pool), 2))
    declared_hosts = []
    seen = set()
    lines = []
    section = None
    for line in base.splitlines():
        stripped = line.strip()
        if stripped.startswith('['):
            section = stripped
            if section == '[switches]':
                raise ValueError('K8 base topology cannot contain switches')
        elif stripped and not stripped.startswith(('#', ';')):
            if section == '[nodes]':
                declared_hosts.append(stripped.split(':', 1)[0])
            elif section == '[links]':
                link, *params = stripped.split()
                pair = tuple(sorted(link.split(':')))
                if pair not in expected_links or pair in seen or not params:
                    raise ValueError('invalid or duplicate K8 link: ' + link)
                seen.add(pair)
                if pair in shaped_links:
                    unaffected = [field for field in params
                                  if field.partition('=')[0] not in ('bw', 'delay', 'loss')]
                    line = ' '.join([link, *unaffected, f'bw={bandwidth:g}',
                                     f'delay={delay:g}ms', f'loss={loss:g}'])
        lines.append(line)
    if len(declared_hosts) != 8 or set(declared_hosts) != hosts:
        raise ValueError('K8 base topology must declare exactly the deployment hosts')
    if seen != expected_links:
        raise ValueError('K8 base topology must contain all 28 point-to-point links')
    return '\n'.join(lines) + '\n', sorted(shaped_links)


def derive(root, network, base_topology=None, provider_nodes=(), shaped_provider=None):
    root = root.resolve(strict=True)
    deployment = json.loads((root / 'deployment.json').read_text(encoding='utf-8'))
    platform = deployment['platform']
    bandwidth, delay, loss = NETWORKS[network]
    topology = root / 'minindn-topology.conf'
    receipt = {'cell': network}
    if (base_topology is not None and not provider_nodes and
            len(deployment['nodes']) == 7):
        # Seven-node star: the two Provider access links are the shaped set;
        # N8 shapes only the selected Provider's access link.
        if platform.get('route_metric') != 'face-cost' or deployment.get('shaping'):
            raise ValueError('star requires face-cost routing and static shaping in the topology')
        if base_topology.resolve(strict=True) == topology.resolve():
            raise ValueError('star base topology must be separate from the generated cell topology')
        providers = {actor['node'] for actor in deployment['actors']
                     if actor['kind'] == 'provider'}
        if len(providers) != 2:
            raise ValueError('seven-node star needs exactly two Provider actors')
        if network == 'N8':
            if shaped_provider not in providers:
                raise ValueError('N8 needs --shape-provider naming one Provider node')
            shaped = {shaped_provider}
        else:
            shaped = providers
        source = base_topology.read_bytes()
        generated, relay = _derive_star_base(source.decode('utf-8'), deployment,
                                             bandwidth, delay, loss, shaped)
        receipt.update(topology_kind='star', shaped_links=sorted(shaped), relay=relay)
    elif base_topology is None:
        if platform.get('route_metric') == 'face-cost' or provider_nodes:
            raise ValueError('K8 cells require --base-topology and four --provider-node IDs')
        providers = {actor['node'] for actor in deployment['actors']
                     if actor['kind'] == 'provider'}
        bandwidth = HISTORICAL_BANDWIDTHS.get(network, bandwidth)
        delay = delay if bandwidth is not None else None
        loss = loss or None
        source = HISTORICAL_STAR.read_bytes()
        generated = _derive_star(source.decode('utf-8'), providers, bandwidth, delay, loss)
        receipt['topology_kind'] = 'historical-star'
    else:
        if platform.get('route_metric') != 'face-cost' or deployment.get('shaping'):
            raise ValueError('K8 requires face-cost routing and static shaping in the topology')
        if base_topology.resolve(strict=True) == topology.resolve():
            raise ValueError('K8 base topology must be separate from the generated cell topology')
        source = base_topology.read_bytes()
        generated, shaped = _derive_mesh(source.decode('utf-8'), deployment, provider_nodes,
                                         bandwidth, delay, loss)
        receipt.update(topology_kind='k8', shaped_links=shaped)

    encoded = generated.encode('utf-8')
    topology.write_bytes(encoded)
    platform['topology'] = str(topology)
    platform['topology_sha256'] = 'sha256:' + hashlib.sha256(encoded).hexdigest()
    (root / 'deployment.json').write_text(
        json.dumps(deployment, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    receipt.update(bandwidth=bandwidth, delay=delay, loss=loss,
                   topology_sha256=platform['topology_sha256'],
                   base_topology_sha256='sha256:' + hashlib.sha256(source).hexdigest())
    with open(root / 'cells.jsonl', 'a', encoding='utf-8') as record:
        record.write(json.dumps(receipt, sort_keys=True) + '\n')
    print(json.dumps({'cell': network, 'topology': str(topology),
                      'topology_sha256': platform['topology_sha256']},
                     sort_keys=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--network', required=True, choices=sorted(NETWORKS))
    parser.add_argument('--base-topology', type=Path,
                        help='Immutable K8 topology. Omit only for historical star deployments.')
    parser.add_argument('--shape-provider',
                        help='N8: shape only this Provider\'s access link')
    parser.add_argument('--provider-node', action='append', default=[],
                        help='Repeat for each of the four K8 Provider-pool node IDs.')
    args = parser.parse_args()
    try:
        derive(args.root, args.network, args.base_topology, args.provider_node,
           args.shape_provider)
    except (ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
