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
from pathlib import Path

CHECKOUT = Path(__file__).resolve().parents[1]
BASE_TOPOLOGY = CHECKOUT / 'tests/fixtures/spec242/minindn-topology.conf'

NETWORKS = {
    'N0': (None, None, None),
    'N1': (10.0, 2.0, None),
    'N2': (1000.0, 2.0, None),
    'N3': (1000.0, 10.0, None),
    'N4': (1000.0, 30.0, None),
    'N5': (1000.0, 2.0, 0.1),
    'N6': (1000.0, 2.0, 1.0),
    'N7': (10.0, 10.0, 0.1),
}


def derive(root, network):
    deployment = json.loads((root / 'deployment.json').read_text())
    providers = {actor['node'] for actor in deployment['actors']
                 if actor['kind'] == 'provider'}
    bandwidth, delay, loss = NETWORKS[network]
    lines = []
    for line in BASE_TOPOLOGY.read_text().splitlines():
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
    topology = root / 'minindn-topology.conf'
    topology.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    deployment['platform']['topology'] = str(topology)
    deployment['platform']['topology_sha256'] = (
        'sha256:' + hashlib.sha256(topology.read_bytes()).hexdigest())
    (root / 'deployment.json').write_text(
        json.dumps(deployment, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    with open(root / 'cells.jsonl', 'a', encoding='utf-8') as record:
        record.write(json.dumps({'cell': network, 'bandwidth': bandwidth,
                                 'delay': delay, 'loss': loss,
                                 'topology_sha256': deployment['platform']['topology_sha256']},
                                sort_keys=True) + '\n')
    print(json.dumps({'cell': network, 'topology': str(topology),
                      'topology_sha256': deployment['platform']['topology_sha256']},
                     sort_keys=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--network', required=True, choices=sorted(NETWORKS))
    args = parser.parse_args()
    derive(args.root, args.network)


if __name__ == '__main__':
    main()
