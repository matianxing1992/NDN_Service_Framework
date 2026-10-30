"""MiniNDN topology facilities for the shared installed native deployment owner.

No task, authorization, planning or inference decisions belong in this module.
Imports are lazy so host and Slurm installations do not require MiniNDN.
"""
from __future__ import annotations

import contextlib
from functools import partial
import hashlib
import heapq
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import threading
import time

from . import cluster
from .processes import owned_state, process_identity, stop_owned, write_json


@contextlib.contextmanager
def _bounded(deadline):
    """Bound synchronous topology-library operations on the CLI's main thread."""
    if threading.current_thread() is not threading.main_thread():
        raise cluster.ClusterError('MININDN_REQUIRES_MAIN_THREAD')
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise cluster.ClusterError('MININDN_FACILITY_TIMEOUT')

    def expired(signum, frame):
        raise cluster.ClusterError('MININDN_FACILITY_TIMEOUT')

    previous = signal.signal(signal.SIGALRM, expired)
    timer = signal.setitimer(signal.ITIMER_REAL, min(30, remaining))
    try:
        yield
    finally:
        signal.setitimer(signal.ITIMER_REAL, *timer)
        signal.signal(signal.SIGALRM, previous)


class MiniNdnFacility:
    """Own one topology and its bound shells while cluster owns native roles."""

    def __init__(self, config, root, state, deadline):
        self.config, self.root, self.state, self.deadline = config, root, state, deadline
        self.network = None
        self.hosts = {}
        self.children = []
        self.record = {'schema': 'ndnsf-di-minindn-facility-v1', 'status': 'STARTING',
                       'nodes': {}, 'launches': [], 'cleanup': [],
                       'hosts': [node['host'] for node in config['nodes']],
                       'creation_started': False}
        state['facility'] = self.record
        self._save()

    def _save(self):
        write_json(self.root / 'state.json', self.state)

    def _capture(self, row, child):
        deadline = min(self.deadline, time.monotonic() + 2)
        while True:
            try:
                proof = process_identity(child.pid)
                if (proof and proof.get('exe') == row['expected_exe'] and
                        proof.get('bindings') == row['bindings'] and owned_state(proof) == 'RUNNING'):
                    namespace = os.readlink('/proc/' + str(child.pid) + '/ns/net')
                    row.update(proof=proof, network_namespace=namespace)
                    self._save()
                    return
            except (OSError, ValueError, IndexError, UnicodeError):
                pass
            if child.poll() is not None or time.monotonic() >= deadline:
                raise cluster.ClusterError('MININDN_NAMESPACE_IDENTITY:' + row['id'])
            time.sleep(0.01)

    def start(self, guard):
        if os.getuid() != 0:
            raise cluster.ClusterError('MININDN_REQUIRES_ROOT')
        try:
            from mininet.node import Host
            from minindn.minindn import Minindn
        except ImportError as error:
            raise cluster.ClusterError('MININDN_FACILITY_UNAVAILABLE:' + str(error)) from error
        topology = Path(self.config['platform']['topology'])
        if 'sha256:' + hashlib.sha256(topology.read_bytes()).hexdigest() != self.config[
                'platform']['topology_sha256']:
            raise cluster.ClusterError('MININDN_TOPOLOGY_CHANGED')
        names = {node['host'] for node in self.config['nodes']}
        # MiniNDN link creation may replace an identically named interface.
        # Reject an existing topology before creating any local facility.
        for path in Path('/proc').iterdir():
            if not path.name.isdigit():
                continue
            try:
                arguments = (path / 'cmdline').read_bytes().split(b'\0')
            except FileNotFoundError:
                continue
            if any(('mininet:' + name).encode() in arguments for name in names):
                raise cluster.ClusterError('MININDN_HOST_ALREADY_ACTIVE')
        probe = subprocess.run(['ip', '-j', 'link', 'show'], capture_output=True,
                               stdin=subprocess.DEVNULL, check=True, timeout=5, text=True)
        for interface in json.loads(probe.stdout):
            if any(interface['ifname'].startswith(name + '-eth') for name in names):
                raise cluster.ClusterError('MININDN_INTERFACE_ALREADY_ACTIVE')
        work = self.root / 'minindn-work'
        cluster._private_directory(work)
        facility = self
        sockets = {node['host']: node['nfd']['socket'] for node in self.config['nodes']}

        class BoundHost(Host):
            def terminate(self):
                row = next((row for row, child in facility.children if child.pid == self.pid), None)
                if not row or not cluster._valid_intent(row, facility.state):
                    raise cluster.ClusterError('MININDN_NAMESPACE_CLOSE_IDENTITY')
                if owned_state(row.get('proof')) == 'RUNNING':
                    try:
                        self.write('exit\n')
                        self.shell.wait(timeout=1)
                    except (OSError, subprocess.TimeoutExpired):
                        pass
                result = stop_owned(row.get('proof'))
                row['termination'] = result
                facility._save()
                if result['status'] != 'ZERO':
                    raise cluster.ClusterError('MININDN_NAMESPACE_CLOSE_UNKNOWN')
                # Links have already been deleted by Mininet.stop(). The base
                # cleanup now sees an exited shell and cannot signal its PID.
                super().terminate()

            def _popen(self, command, **kwargs):
                if 'env' in kwargs or 'mininet:' + self.name not in command:
                    return super()._popen(command, **kwargs)
                actor = 'namespace.' + self.name
                home = work / self.name
                cluster._private_directory(home)
                env = cluster._environment(home, sockets[self.name], actor, facility.state)
                kwargs['env'] = env
                row = {'id': actor, 'expected_exe': str(Path(shutil.which('bash')).resolve()),
                       'bindings': {key: env[key] for key in (
                           'NDNSF_DEPLOYMENT_ACTOR_ID', 'NDNSF_DEPLOYMENT_BOOT_ID',
                           'NDNSF_DEPLOYMENT_CONFIG_SHA256')}}
                facility.record['launches'].append(row)
                facility._save()
                try:
                    child = super()._popen(command, **kwargs)
                except OSError:
                    row['spawn_failed'] = True
                    facility._save()
                    raise
                facility.children.append((row, child))
                facility._capture(row, child)
                return child

        guard()
        with _bounded(self.deadline):
            parsed, _ = Minindn.processTopo(str(topology))
            if parsed.switches():
                raise cluster.ClusterError('MININDN_POINT_TO_POINT_TOPOLOGY_REQUIRED')
            # The library has no argv parameter. Isolate its CLI parsing and
            # restore caller arguments even if construction is interrupted.
            previous = sys.argv
            try:
                sys.argv = [previous[0]]
                self.record['creation_started'] = True
                self._save()
                self.network = Minindn(topo=parsed, topoFile=str(topology), workDir=str(work),
                                       host=BoundHost, controller=None)
            finally:
                sys.argv = previous
            if self.network.ndnSecurityDisabled:
                raise cluster.ClusterError('MININDN_NATIVE_SECURITY_REQUIRED')
            self.hosts = {host.name: host for host in self.network.net.hosts}
            if set(self.hosts) != names:
                raise cluster.ClusterError('MININDN_TOPOLOGY_HOST_MISMATCH')
            self.network.start()
        for node in self.config['nodes']:
            host = self.hosts[node['host']]
            shell = next(row for row, child in self.children if child.pid == host.pid)
            self.record['nodes'][node['id']] = dict(host=node['host'], shell=shell['id'],
                                                   network_namespace=shell['network_namespace'])
        if len({row['network_namespace'] for row in self.record['nodes'].values()}) != len(names):
            raise cluster.ClusterError('MININDN_NAMESPACE_NOT_DISTINCT')
        self.record['status'] = 'STARTED'
        self._save()
        guard()

    def namespace(self, node_id):
        return self.record['nodes'][node_id]['network_namespace']

    def launcher(self, node_id):
        host = next(node['host'] for node in self.config['nodes'] if node['id'] == node_id)
        node = self.hosts[host]
        # Popen owns setsid(). MiniNDN's default mnexec -d would fork again
        # when that child is already a group leader, losing the native PID.
        return partial(node.popen, mncmd=['mnexec', '-a', str(node.pid)], start_new_session=True)

    def routes(self, directory, guard):
        # MiniNDN's routing helper launches unchecked shell commands on a thread
        # pool. Keep topology routing here as bounded, checked nfdc commands.
        nodes = {node['id']: node for node in self.config['nodes']}
        graph = {node_id: {} for node_id in nodes}
        ids = {node['host']: node['id'] for node in nodes.values()}
        for node_id, node in nodes.items():
            for interface in self.hosts[node['host']].intfList():
                if not interface.link:
                    continue
                link = interface.link
                other = link.intf2 if link.intf1 is interface else link.intf1
                # Link delay is the same weight used by MiniNDN's maintained
                # link-state helper. All commands use the destination's link IP.
                delay = float(str(interface.params.get('delay', '1ms')).replace('ms', ''))
                graph[node_id][ids[other.node.name]] = (delay, other.IP())
        origins = []
        for actor in self.config['actors']:
            if not actor['external']:
                origins.extend([(actor['node'], actor['identity']),
                                (actor['node'], actor['identity'] + '/KEY')])
        origins.extend((route['to'], route['prefix']) for route in self.config['network']['routes'])
        origins.extend((node_id, self.config['network']['group']) for node_id in nodes)
        commands = []

        def apply(source, argv):
            guard()
            child = self.launcher(source)(argv, env=cluster._environment(
                self.root / ('nfdc-' + source), nodes[source]['nfd']['socket'],
                'routes.' + source, self.state), stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            try:
                output, _ = child.communicate(timeout=min(5, max(
                    0.01, self.deadline - time.monotonic())))
            finally:
                if child.poll() is None:
                    child.kill()
                child.wait()
            text = output.decode('utf-8', errors='replace')
            commands.append(dict(source=source, arguments=argv, exit_code=child.returncode, output=text))
            write_json(directory / 'routes.json', {'commands': commands})
            if child.returncode:
                raise cluster.ClusterError('MININDN_ROUTE_FAILED:' + source)
            return text

        with _bounded(self.deadline):
            for source, node in nodes.items():
                faces = {}
                distances, queue = {}, [(0, source, '')]
                while queue:
                    cost, current, hop = heapq.heappop(queue)
                    if current in distances:
                        continue
                    distances[current] = (cost, hop)
                    for neighbor, (weight, address) in graph[current].items():
                        if neighbor not in distances:
                            heapq.heappush(queue, (cost + weight, neighbor,
                                                  address if current == source else hop))
                for destination, prefix in dict.fromkeys(origins):
                    if destination == source:
                        continue
                    if destination not in distances:
                        raise cluster.ClusterError('MININDN_ROUTE_UNREACHABLE:' + destination)
                    cost, address = distances[destination]
                    hop_node = next(neighbor for neighbor, (_, ip) in graph[source].items()
                                    if ip == address)
                    if hop_node not in faces:
                        uri = 'udp://' + address + ':' + str(nodes[hop_node]['nfd']['port'])
                        text = apply(source, ['nfdc', 'face', 'create', uri, 'persistency', 'permanent'])
                        match = re.search(r'face-(?:created|exists|updated) id=([0-9]+)', text)
                        if not match:
                            raise cluster.ClusterError('MININDN_FACE_ID_MISSING:' + source)
                        faces[hop_node] = match.group(1)
                    apply(source, ['nfdc', 'route', 'add', prefix, faces[hop_node],
                                   'cost', str(max(1, int(cost)))])
        guard()

    def close(self):
        return close_facility(self.state, self.root, self.network, self.children)


def close_facility(state, root, network=None, children=()):
    """Close only persisted bound shell groups; no global MiniNDN cleanup."""
    record = state.get('facility')
    if not record:
        return True
    previously_closed = (record.get('status') == 'STOPPED' and not record.get('failure') and
                         record.get('namespace_survivors') == [] and record.get('remaining_interfaces') == [])
    previous = {row['id']: row for row in record.get('cleanup', [])}
    error = None
    if network is not None and not previously_closed:
        try:
            # Link deletion needs live namespace shells. BoundHost.terminate
            # closes each shell only after those link operations have drained.
            with _bounded(time.monotonic() + 15):
                network.stop()
        except BaseException as failure:
            error = str(failure) or type(failure).__name__
    cleanup = []
    for row in reversed(record['launches']):
        if not cluster._valid_intent(row, state):
            result = {'status': 'UNKNOWN', 'error': 'namespace launch binding mismatch'}
        else:
            cluster._recover(row)
            result = ({'status': 'ZERO'} if row.get('spawn_failed') and not row.get('proof')
                      else stop_owned(row.get('proof')))
        if previous.get(row['id'], {}).get('forced'):
            result['forced'] = True
        if row.get('termination', {}).get('forced'):
            result['forced'] = True
        for key in ('exitCode', 'reaped'):
            if key not in result:
                if key in row.get('termination', {}):
                    result[key] = row['termination'][key]
                elif key in previous.get(row['id'], {}):
                    result[key] = previous[row['id']][key]
        cleanup.append(dict(result, id=row['id']))
        record['cleanup'] = cleanup
        write_json(root / 'state.json', state)
    for _, child in children:
        child.poll()
    if previously_closed and not error and all(row['status'] == 'ZERO' for row in cleanup):
        # Kernel namespace inode numbers and interface names may be reused by
        # a later independent topology. Recheck owned groups, not that new
        # topology, when this facility already has a complete closeout proof.
        write_json(root / 'state.json', state)
        return True
    try:
        namespaces = {row.get('network_namespace') for row in record['launches']}
        namespaces.discard(None)
        survivors = []
        for path in Path('/proc').iterdir() if namespaces else ():
            if not path.name.isdigit():
                continue
            try:
                if os.readlink(path / 'ns/net') in namespaces:
                    survivors.append(int(path.name))
            except FileNotFoundError:
                continue
        interfaces = [path.name for path in Path('/sys/class/net').iterdir()
                      if any(path.name.startswith(name + '-eth') for name in record['hosts'])]
        record['namespace_survivors'] = survivors
        record['remaining_interfaces'] = interfaces
        if survivors or interfaces:
            error = error or 'MININDN_FACILITY_SURVIVORS'
    except OSError as failure:
        error = str(failure)
    record['failure'] = error
    closed = (not error and all(row['status'] == 'ZERO' for row in cleanup))
    record['status'] = 'STOPPED' if closed else 'UNKNOWN'
    write_json(root / 'state.json', state)
    return closed
