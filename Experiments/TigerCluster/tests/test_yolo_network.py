"""Actual RSA signing/verification over an in-memory Interest/Data test Face.

No NFD, SIF, physical cross-node routing or model qualification is claimed.
"""
import asyncio
import json
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest
from Cryptodome.PublicKey import RSA
from ndn.encoding import Name, MetaInfo, make_data, parse_data
from ndn.security.signer import Sha256WithRsaSigner
from ndn.types import InterestTimeout, ValidationFailure

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from apps import yolo, yolo_network as network
from runtime.yolo_worker import NodeRuntime
from test_yolo_worker import prepared


@pytest.fixture(scope='module')
def signed_material():
    roles = ('root', *network.ROLES)
    keys = {r: RSA.generate(2048) for r in roles}
    names = {r: '/run/test' + ('' if r == 'root' else '/' + r) + '/KEY/key' for r in roles}
    signers = {r: Sha256WithRsaSigner(names[r], keys[r].export_key(format='DER')) for r in roles}
    certs = {r: make_data(names[r] + '/issuer/v=1', MetaInfo(content_type=2),
                          keys[r].public_key().export_key(format='DER'), signers['root']) for r in roles}
    return signers, certs


@pytest.mark.parametrize('fault', ['none', 'signature', 'payload', 'name', 'certificate', 'missing', 'outer-timeout'])
def test_bidirectional_fresh_signed_exchange(signed_material, fault, monkeypatch):
    if fault == 'outer-timeout':
        async def expire(coro, *, timeout):
            coro.close()
            raise asyncio.TimeoutError()
        monkeypatch.setattr(network.asyncio, 'wait_for', expire)
    signers, certs = signed_material
    callbacks = {}
    class Face:
        wire = None
        async def register(self, name, callback):
            callbacks[name] = (self, callback)
            return True
        def prepare_data(self, name, **kwargs):
            kwargs.pop('freshness_period')
            if fault == 'name':
                name = '/wrong/name'
            if fault == 'payload':
                kwargs['content'] = b'old probe'
            return make_data(name, MetaInfo(freshness_period=0), **kwargs)
        def put_raw_packet(self, wire):
            if fault == 'signature':
                wire = bytearray(wire)
                wire[-1] ^= 1
            self.wire = wire
        async def express_interest(self, name, **kwargs):
            assert kwargs['must_be_fresh'] is True
            if fault == 'missing' or name not in callbacks:
                raise InterestTimeout()
            face, callback = callbacks[name]
            callback(Name.from_str(name), None, None)
            parsed_name, meta, content, pointers = parse_data(face.wire)
            if not await kwargs['validator'](parsed_name, pointers):
                raise ValidationFailure(parsed_name, meta, content)
            return parsed_name, meta, content
    def read(role):
        wire = certs[role]
        if fault == 'certificate' and role != 'root':
            wire = bytearray(wire)
            wire[-1] ^= 1
        return wire
    async def run():
        return await asyncio.gather(*[
            network.exchange(Face(), SimpleNamespace(get_signer=lambda _, r=r: signers[r]),
                             '/run/test', r, 'a' * 32, 0.2, read)
            for r in network.ROLES], return_exceptions=True)
    results = asyncio.run(run())
    if fault == 'none':
        assert all(isinstance(r, dict) and r['status'] == 'READY' for r in results)
        assert results[0]['peer'] == results[1]['producer']
        assert results[1]['peer'] == results[0]['producer']
    else:
        expected_error = {'signature': ValidationFailure, 'payload': ValueError,
                          'name': ValueError, 'certificate': ValueError, 'missing': TimeoutError,
                          'outer-timeout': TimeoutError}[fault]
        assert all(isinstance(r, expected_error) for r in results), results


@pytest.mark.parametrize('rank', [0, 1])
def test_network_probe_releases_provider_home_before_startup(tmp_path, monkeypatch, rank):
    worker = NodeRuntime(**prepared(tmp_path, rank=rank))
    worker._preparation_binding = ('fixture',)
    monkeypatch.setattr(worker, '_verify_prepared_boundary', lambda: None)
    original, calls = subprocess.Popen, []
    def boundary(argv, **kwargs):
        calls.append(argv)
        return original([sys.executable, '-c', 'pass'], **kwargs)
    monkeypatch.setattr(subprocess, 'Popen', boundary)
    role = network.ROLES[rank]
    try:
        assert worker.run_network_probe(['python3', '-m', 'apps.yolo_network'], seconds=3) == 0
        assert worker.leases == {}
        assert role not in worker.started
        assert '--nv' not in calls[0]
        assert not any(':/artifacts:' in arg for arg in calls[0])
        assert calls[0][calls[0].index('--home') + 1] == '/identities/' + role
        assert all(not arg.startswith(str(home) + ':')
                   for r, home in worker.homes.items() if r != role for arg in calls[0])
        with pytest.raises(ValueError, match='REUSED'):
            worker.run_network_probe(['python3'], seconds=3)
        # Borrowing the HOME must not mark the actual service as already started.
        worker.start_service(role, ['python3'])
        assert role in worker.started
        with pytest.raises(ValueError, match='ROLE'):
            worker.run_network_probe(['python3'], seconds=3)
    finally:
        assert all(r['reaped'] and not r['forced'] for r in worker.close())


@pytest.mark.parametrize('fault', ['none', 'stale', 'wrong-peer', 'exit'])
def test_parent_requires_exact_peer_receipt(tmp_path, fault):
    plan = dict(namespace='/run/test', identities={r: '/run/test/' + r for r in network.ROLES})
    worker = SimpleNamespace(rank=1, mode='two-node-gpu', output=tmp_path, cleanup_seconds=2,
        _preparation_binding=(plan,), _verify_prepared_boundary=lambda: None, check=lambda: None)
    def run(argv, **kwargs):
        assert kwargs['seconds'] == 3
        assert argv[argv.index('--identity') + 1] == plan['identities']['DetectShard0']
        assert argv[argv.index('--peer-identity') + 1] == plan['identities']['BackboneNeck']
        receipt = dict(schema='tiger-yolo-network-readiness-v1', probeId='a' * 32,
            producer='/run/test/DetectShard0', peer='/run/test/BackboneNeck',
            receivedName='/run/test/BackboneNeck/SPEC183-NETWORK/' + 'a' * 32 + '/data',
            status='READY', qualification='NOT_EVALUATED')
        if fault == 'stale':
            receipt['probeId'] = 'b' * 32
        if fault == 'wrong-peer':
            receipt['peer'] = '/foreign'
        path = tmp_path / 'DetectShard0/requests/network-readiness/receipt.json'
        path.parent.mkdir(parents=True)
        path.write_text(json.dumps(receipt))
        return 2 if fault == 'exit' else 0
    worker.run_network_probe = run
    if fault == 'none':
        assert yolo.wait_network_ready(worker, probe_id='a' * 32, seconds=1)['status'] == 'READY'
    else:
        with pytest.raises((RuntimeError, ValueError)):
            yolo.wait_network_ready(worker, probe_id='a' * 32, seconds=1)


def test_probe_names_preserve_prepared_identity_not_role_spelling():
    names = dict(BackboneNeck='/run/test/custom-provider', DetectShard0='/run/test/other/head')
    own, peer, own_name, peer_name = network.probe_names('/run/test', 'BackboneNeck', 'a' * 32, names)
    assert own == names['BackboneNeck'] and peer == names['DetectShard0']
    assert own_name.startswith(own + '/SPEC183-NETWORK/')
    assert peer_name.startswith(peer + '/SPEC183-NETWORK/')
