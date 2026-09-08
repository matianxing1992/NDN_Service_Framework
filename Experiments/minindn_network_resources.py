"""Read-only kernel evidence for resources owned by one MiniNDN instance."""
from __future__ import annotations

import json
import math
import os
from pathlib import Path
import re
import socket
import time


def write_exclusive(path, value):
    descriptor = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, 'w') as stream:
        json.dump(value, stream, sort_keys=True, indent=2)
        stream.write('\n')


def _namespace(path):
    value = os.readlink(str(path))
    if not re.fullmatch(r'net:\[\d+\]', value):
        raise ValueError('NETWORK_NAMESPACE_IDENTITY')
    return value


def capture(network, *, proc_root=Path('/proc'), interfaces=None):
    """Capture actual node PIDs/namespaces and interfaces in our namespace."""
    observer = _namespace(proc_root/'self/ns/net')
    links = dict(socket.if_nameindex() if interfaces is None else interfaces)
    by_name = {name: index for index, name in links.items()}
    hosts = list(network.net.hosts)
    switches = list(getattr(network.net, 'switches', []))
    controllers = list(getattr(network.net, 'controllers', []))
    nodes = {node.name: node for node in hosts + switches + controllers}
    if not nodes:
        raise ValueError('NETWORK_RESOURCE_NODE_SET_EMPTY')
    owned_links, rows = {}, []
    for name, node in sorted(nodes.items()):
        if type(node.pid) is not int or node.pid <= 0:
            raise ValueError('NETWORK_RESOURCE_PID')
        root = proc_root/str(node.pid)
        start = int((root/'stat').read_text().rsplit(')', 1)[1].split()[19])
        namespace = _namespace(root/'ns/net')
        rows.append(dict(node=name, pid=node.pid, startTicks=start, namespace=namespace))
        # Interface names only identify owned root links when observed in the
        # root namespace; private-node names may coincide with unrelated links.
        if namespace == observer:
            for intf in node.intfList():
                if intf.name != 'lo' and intf.name in by_name:
                    owned_links[intf.name] = by_name[intf.name]
        if node in switches and name in by_name:
            owned_links[name] = by_name[name]
    return dict(schema='minindn-network-resources-v1', observerNamespace=observer,
                nodes=rows, namespaces=sorted({r['namespace'] for r in rows if r['namespace'] != observer}),
                interfaces=[dict(name=name, ifindex=index) for name,index in sorted(owned_links.items())])


def combine(snapshots):
    """Union successive inventories before one kernel scan at teardown."""
    observers = {row['observerNamespace'] for row in snapshots}
    if len(observers) != 1:
        raise ValueError('NETWORK_OBSERVER_CHANGED')
    links = {(row['ifindex'], row['name']) for snapshot in snapshots for row in snapshot['interfaces']}
    return dict(observerNamespace=next(iter(observers)),
                namespaces=sorted({n for snapshot in snapshots for n in snapshot['namespaces']}),
                interfaces=[dict(ifindex=index, name=name) for index,name in sorted(links)])


def inspect(resources, *, proc_root=Path('/proc'), interfaces=None, seconds=5):
    """Find surviving namespace references/root links; never delete resources.

    Inspect current namespaces, open namespace FDs and nsfs bind mounts.
    Permission or observation failures are not evidence of cleanup.
    """
    if (isinstance(seconds, bool) or not isinstance(seconds, (int, float))
            or not math.isfinite(seconds) or seconds <= 0):
        raise ValueError('NETWORK_OBSERVATION_BUDGET')
    deadline = time.monotonic() + seconds
    def bounded():
        if time.monotonic() >= deadline:
            raise RuntimeError('NETWORK_OBSERVATION_TIMEOUT')
    if _namespace(proc_root/'self/ns/net') != resources['observerNamespace']:
        raise ValueError('NETWORK_OBSERVER_CHANGED')
    wanted = set(resources['namespaces'])
    references = []
    for process in proc_root.iterdir():
        bounded()
        if not process.name.isdecimal():
            continue
        try:
            for task in (process/'task').iterdir():
                bounded()
                if not task.name.isdecimal():
                    continue
                try:
                    namespace = _namespace(task/'ns/net')
                    if namespace in wanted:
                        references.append(dict(kind='process', pid=int(process.name),
                                               tid=int(task.name), namespace=namespace))
                    for fd in (task/'fd').iterdir():
                        bounded()
                        try:
                            target = os.readlink(str(fd))
                        except FileNotFoundError:
                            continue
                        if target in wanted:
                            references.append(dict(kind='fd', pid=int(process.name),
                                tid=int(task.name), fd=fd.name, namespace=target))
                except FileNotFoundError:
                    continue
        except FileNotFoundError:
            # Processes/fds may exit during the observation. Permission and
            # other errors deliberately propagate instead of implying absence.
            continue
    inode_names = {int(value[5:-1]): value for value in wanted}
    for line in (proc_root/'self/mountinfo').read_text().splitlines():
        bounded()
        left, right = line.split(' - ', 1)
        if right.split()[0] != 'nsfs':
            continue
        target = re.sub(r'\\([0-7]{3})', lambda m: chr(int(m.group(1), 8)), left.split()[4])
        try:
            inode = os.stat(target).st_ino
        except FileNotFoundError:
            continue
        if inode in inode_names:
            references.append(dict(kind='mount', path=target, namespace=inode_names[inode]))
    links = dict(socket.if_nameindex() if interfaces is None else interfaces)
    remaining = [row for row in resources['interfaces'] if links.get(row['ifindex']) == row['name']]
    return dict(schema='minindn-network-observation-v1', references=references,
                interfaces=remaining, clean=not references and not remaining,
                qualification='NOT_EVALUATED')
