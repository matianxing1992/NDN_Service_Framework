#!/usr/bin/env python3
"""Replay an original signed Wan response segment on the Requester's local Face.

Run with the installed NDNSF Python interpreter and a completed warm TP fixture.
The relay only controls delivery. The installed C++ Requester, Providers and
oracle own request admission, numerical comparison and clip verification.
"""
import argparse
import asyncio
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import threading
from unittest.mock import patch

from ndn.encoding import Name, parse_data, parse_lp_packet, parse_tl_num
from ndnsf_distributed_inference.ops import cluster


SEGMENT_ZERO = re.compile(
    r'/NDNSF/LARGE-RESPONSE/.+/NDNSF/DI/REQUEST/([0-9a-f]+)-(3|4)/[^/]+/v=[0-9]+/seg=0$'
)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


async def read_packet(reader):
    async def number():
        first = await reader.readexactly(1)
        width = {253: 2, 254: 4, 255: 8}.get(first[0], 0)
        return first + await reader.readexactly(width)

    header = await number()
    length = await number()
    count, _ = parse_tl_num(length)
    require(count <= 8800, 'local Face packet exceeds fixture limit')
    return header + length + await reader.readexactly(count)


def response_segment(wire):
    if parse_tl_num(wire)[0] == 100:
        _, wire = parse_lp_packet(wire)
    if not wire or parse_tl_num(wire)[0] != 6:
        return None
    name, _, _, signature = parse_data(wire)
    name = Name.to_str(name)
    coordinate = SEGMENT_ZERO.search(name)
    if coordinate is None:
        return None
    require(signature.signature_value_buf and signature.signature_info.key_locator,
            f'expected a signed response segment: {name}')
    return coordinate.groups(), name, bytes(wire)


class ReplayRelay:
    def __init__(self, socket, clip):
        self.socket = socket
        self.clip = clip
        self.upstream = None
        self.old = None
        self.receipt = None
        self.error = None
        self.ready = threading.Event()
        self.loop = asyncio.new_event_loop()
        self.worker = threading.Thread(target=self.run)
        self.connections = set()

    def run(self):
        asyncio.set_event_loop(self.loop)
        try:
            self.loop.run_until_complete(self.serve())
        except Exception as error:
            self.error = error
            self.ready.set()
        finally:
            self.loop.close()

    async def serve(self):
        async with await asyncio.start_unix_server(self.connect, path=str(self.socket)):
            self.stop = asyncio.Event()
            self.ready.set()
            await self.stop.wait()
        pending = list(self.connections)
        for task in pending:
            task.cancel()
        await asyncio.gather(*pending, return_exceptions=True)

    async def connect(self, client_read, client_write):
        if len(self.connections) >= 8:
            self.error = RuntimeError('unexpected number of Requester Face connections')
            client_write.close()
            return
        connection = asyncio.current_task()
        self.connections.add(connection)
        upstream_write = None
        try:
            upstream_read, upstream_write = await asyncio.open_unix_connection(self.upstream)
            pipes = (
                asyncio.create_task(self.relay(client_read, upstream_write)),
                asyncio.create_task(self.relay(upstream_read, client_write, toward_client=True)),
            )
            try:
                done, _ = await asyncio.wait(pipes, return_when=asyncio.FIRST_COMPLETED)
                for task in done:
                    task.result()
            finally:
                for task in pipes:
                    task.cancel()
                await asyncio.gather(*pipes, return_exceptions=True)
        except (asyncio.IncompleteReadError, BrokenPipeError, ConnectionResetError):
            pass
        except Exception as error:
            self.error = error
        finally:
            self.connections.discard(connection)
            client_write.close()
            if upstream_write is not None:
                upstream_write.close()

    async def relay(self, reader, writer, *, toward_client=False):
        while True:
            packet = await read_packet(reader)
            if toward_client:
                segment = response_segment(packet)
                if segment is not None:
                    await self.replay(writer, segment)
            writer.write(packet)
            await writer.drain()

    async def replay(self, writer, segment):
        (run_id, child), name, wire = segment
        if child == '3' and self.old is None:
            self.old = run_id, name, wire
        if child != '4' or self.receipt is not None:
            return
        require(self.old and self.old[0] == run_id and self.old[1] != name,
                'missing distinct signed response for the preceding child')
        _, stale_name, stale_wire = self.old
        # The fourth child's segment is already pending on the same Face.
        # Deliver the prior signed Data unchanged before releasing this frame.
        writer.write(stale_wire)
        await writer.drain()
        await asyncio.sleep(0.25)
        stages = list(self.clip.parent.glob(self.clip.name + '-*'))
        require(len(stages) == 1 and not self.clip.exists(), 'unexpected clip publication')
        traces = sorted(path.name for path in stages[0].glob('trace-*.bundle'))
        require(traces == ['trace-0.bundle', 'trace-1.bundle', 'trace-2.bundle'],
                f'Wan advanced before the current response: {traces}')
        require((stages[0] / 'frames.rgb24').stat().st_size == 0, 'RGB staged before current response')
        self.receipt = dict(stale=stale_name, pending=name, staged=traces,
                            stale_sha256=hashlib.sha256(stale_wire).hexdigest(),
                            pending_sha256=hashlib.sha256(wire).hexdigest())

    def __enter__(self):
        self.worker.start()
        require(self.ready.wait(5) and self.error is None, f'relay did not start: {self.error}')
        return self

    def __exit__(self, *exception):
        self.loop.call_soon_threadsafe(self.stop.set)
        self.worker.join(timeout=5)
        require(not self.worker.is_alive(), 'test relay failed to drain')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', type=Path, required=True)
    parser.add_argument('--cluster-cli', type=Path, help='installed ndnsf-di host launcher')
    parser.add_argument('--warm-deployment', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, required=True)
    args = parser.parse_args()
    prefix = args.prefix.resolve(strict=True)
    cli = (args.cluster_cli or prefix / 'libexec/ndnsf-python/bin/ndnsf-di').resolve(strict=True)
    warm = args.warm_deployment.resolve(strict=True)
    fixture = args.fixture.resolve(strict=True)
    config = json.loads(warm.read_text())
    requester = config['requester']
    require(config['deployment_id'] == 'wan-video-tp' and requester['test_only'] and
            requester['test_wan_fixture_trace'] and
            requester['model_input']['mode'] in ('source-cache', 'stored'),
            'expected the completed warm TP fixture')
    run = Path(tempfile.mkdtemp(prefix='wan-replay-', dir=warm.parent))
    clip = run / 'clip'
    requester['wan_generation']['output_directory'] = str(clip)
    requester['wan_generation']['generation_id'] += '-replay'
    source = warm.with_name(run.name + '.json')
    with source.open('x') as output:
        json.dump(config, output, sort_keys=True)
    native_launch = cluster._native_launch

    subprocess.run([str(cli), 'cluster', 'check',
                    '--config', str(source)], timeout=30, check=True, stdout=subprocess.DEVNULL)
    with tempfile.TemporaryDirectory(prefix='wan-relay-') as private:
        with ReplayRelay(Path(private) / 'face.sock', clip) as relay:
            def launch(config, actor, directory, state, **kwargs):
                command, env = native_launch(config, actor, directory, state, **kwargs)
                if actor['kind'] == 'requester':
                    transport = env['NDN_CLIENT_TRANSPORT']
                    require(transport.startswith('unix://'), 'expected the local Unix Face')
                    relay.upstream = transport[len('unix://'):]
                    env['NDN_CLIENT_TRANSPORT'] = 'unix://' + str(relay.socket)
                if actor['kind'] == 'provider':
                    # Both denoiser outputs must use the real segmented response path.
                    env['NDNSF_RESPONSE_LARGE_DATA_THRESHOLD'] = '256'
                return command, env

            with patch.object(cluster, '_native_launch', launch):
                try:
                    result = cluster.run_cluster('up', source, timeout=150, timing=True)
                finally:
                    down = cluster.run_cluster('down', source, timeout=30)

    require(relay.error is None and relay.receipt, f'replay boundary missing: {relay.error}')
    require(result['boot_id'] == down['boot_id'] and down['failure'] is None,
            'deployment did not finish cleanly')
    facility = down['facility']
    cleanup = down['cleanup'] + facility['cleanup']
    expected_actors = {actor['id'] for actor in config['actors']}
    expected_actors.update('nfd.' + node['id'] for node in config['nodes']
                           if node['nfd']['mode'] == 'managed')
    require(len(down['cleanup']) == len(expected_actors) and
            {row['id'] for row in down['cleanup']} == expected_actors and
            len(facility['cleanup']) == len(config['nodes']) and
            all(row['status'] == 'ZERO' and not row['forced'] for row in cleanup) and
            not facility['namespace_survivors'] and not facility['remaining_interfaces'],
            'expected all actors and namespaces to exit without force')
    receipt = Path(result['evidence_root']) / 'operation.json'
    subprocess.run([str(prefix / 'bin/di-wan-video-oracle'), '--deployment-state', str(receipt),
                    '--fixture', str(fixture)], timeout=60, check=True)
    print(json.dumps(dict(relay.receipt, boot=result['boot_id'], cleanup=len(cleanup),
                          deployment_state=str(receipt)), sort_keys=True))


if __name__ == '__main__':
    main()
