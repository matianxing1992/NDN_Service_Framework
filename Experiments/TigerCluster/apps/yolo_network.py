"""Finite, bidirectional signed Data readiness before native Provider startup.

Both ranks run this command concurrently with one fresh shared probe ID. Each
keeps serving until its bounded observation window ends, even after receiving
its peer's Data. Both independently validated receipts are required; there is
no shared-file activation/data shortcut and no PUBPARAMS probe.
"""
import argparse
import asyncio
import base64
import json
import math
from pathlib import Path
import re

from ndn.app import NDNApp
from ndn.encoding import Name, parse_data
from ndn.security.keychain.keychain_sqlite3 import KeychainSqlite3
from ndn.security.tpm.tpm_file import TpmFile
from ndn.security.validator.known_key_validator import RsaChecker
from ndn.transport.stream_socket import UnixFace
from ndn.types import InterestNack, InterestTimeout

from runtime.identities import _read_credential, _credential_document, identity_inventory


ROLES = ('BackboneNeck', 'DetectShard0')


def probe_names(namespace, role, probe_id, identities=None):
    if role not in ROLES or not isinstance(probe_id, str) or not re.fullmatch(r'[a-f0-9]{32}', probe_id):
        raise ValueError('NETWORK_PROBE_ARGUMENTS')
    names = identity_inventory(namespace, identities if identities is not None else
                               {r: namespace + '/' + r for r in ROLES})
    if set(names) != set(ROLES):
        raise ValueError('NETWORK_PROBE_IDENTITIES')
    peer = ROLES[1 - ROLES.index(role)]
    suffix = '/SPEC183-NETWORK/' + probe_id + '/data'
    return names[role], names[peer], names[role] + suffix, names[peer] + suffix


def certificate(role):
    encoded = _read_credential(Path('/config') / (role + '.cert'))
    return base64.b64decode(b''.join(encoded.split()), validate=True)


async def exchange(app, keys, namespace, role, probe_id, seconds, read_certificate=certificate,
                   identities=None):
    own, peer, own_name, peer_name = probe_names(namespace, role, probe_id, identities)
    if (isinstance(seconds, bool) or not isinstance(seconds, (int, float))
            or not math.isfinite(seconds) or not 0 < seconds <= 120):
        raise ValueError('NETWORK_PROBE_BUDGET')
    loop = asyncio.get_running_loop()
    deadline = loop.time() + seconds
    root_checker = RsaChecker.from_cert(read_certificate('root'))
    peer_role = ROLES[1 - ROLES.index(role)]
    certs = {}
    for identity, cert_role in ((own, role), (peer, peer_role)):
        wire = read_certificate(cert_role)
        cert_name, _, _, pointers = parse_data(wire)
        if (not Name.is_prefix(Name.from_str(identity + '/KEY'), cert_name)
                or not await root_checker(cert_name, pointers)):
            raise ValueError('NETWORK_PROBE_CERTIFICATE')
        certs[identity] = wire
    signer = keys.get_signer({'identity': own})
    content = json.dumps({'probeId': probe_id, 'producer': own}, sort_keys=True).encode()
    def receive(name, _param, _payload):
        if Name.to_str(name) == own_name:
            app.put_raw_packet(app.prepare_data(name, content=content, freshness_period=0, signer=signer))
    if not await app.register(own_name, receive):
        raise RuntimeError('NETWORK_PROBE_REGISTRATION')
    peer_checker = RsaChecker.from_cert(certs[peer])
    expected = json.dumps({'probeId': probe_id, 'producer': peer}, sort_keys=True).encode()
    async def fetch():
        while loop.time() < deadline:
            try:
                name, _, payload = await app.express_interest(peer_name, must_be_fresh=True,
                    lifetime=max(1, min(1000, int((deadline - loop.time()) * 1000))), validator=peer_checker)
                if Name.to_str(name) != peer_name or bytes(payload) != expected:
                    raise ValueError('NETWORK_PROBE_DATA_MISMATCH')
                return
            except (InterestNack, InterestTimeout):
                await asyncio.sleep(min(0.05, max(0, deadline - loop.time())))
        raise TimeoutError('NETWORK_PROBE_PEER_TIMEOUT')
    try:
        await asyncio.wait_for(fetch(), timeout=max(0, deadline - loop.time()))
    except asyncio.TimeoutError as exc:
        # Python 3.8 has a distinct asyncio.TimeoutError; normalize both
        # scheduler expiry and the inner peer deadline at this API boundary.
        raise TimeoutError('NETWORK_PROBE_PEER_TIMEOUT') from exc
    # Do not exit immediately: the opposite rank must still fetch our Data.
    # A delayed/missing rank fails closed; the parent requires both receipts.
    await asyncio.sleep(max(0, deadline - loop.time()))
    return dict(schema='tiger-yolo-network-readiness-v1', probeId=probe_id,
                producer=own, peer=peer, receivedName=peer_name,
                status='READY', qualification='NOT_EVALUATED')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--namespace', required=True)
    parser.add_argument('--role', choices=ROLES, required=True)
    parser.add_argument('--identity', required=True)
    parser.add_argument('--peer-identity', required=True)
    parser.add_argument('--probe-id', required=True)
    parser.add_argument('--seconds', type=float, required=True)
    args = parser.parse_args(argv)
    identities = {args.role: args.identity, ROLES[1 - ROLES.index(args.role)]: args.peer_identity}
    probe_names(args.namespace, args.role, args.probe_id, identities)
    target = Path('/output/requests/network-readiness/receipt.json')
    if target.exists() or any(p.is_symlink() for p in (target, *target.parents)):
        raise ValueError('NETWORK_PROBE_OUTPUT')
    home = Path.home() / '.ndn'
    keys = KeychainSqlite3(str(home / 'pib.db'), TpmFile(str(home / 'ndnsec-key-file')))
    app = NDNApp(face=UnixFace('/node/nfd.sock'), keychain=keys)
    receipts, failures = [], []
    async def work():
        try:
            receipts.append(await exchange(app, keys, args.namespace, args.role, args.probe_id, args.seconds,
                                           identities=identities))
        except BaseException as exc:
            failures.append(type(exc).__name__)
        finally:
            app.shutdown()
    app.run_forever(after_start=work())
    if failures or len(receipts) != 1:
        raise RuntimeError('NETWORK_PROBE_FAILED:' + ','.join(failures))
    _credential_document(target, receipts[0])
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
