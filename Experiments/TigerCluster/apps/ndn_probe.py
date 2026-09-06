"""Real NDN Interest/Data and certificate verification over a job-local Face.

The producer publishes its signed leaf certificate. The consumer fetches it
over NDN and verifies it against the run root before accepting application Data.
Wrong signatures and wrong trust keys must fail validation, never just time out.
"""
import argparse
import asyncio
import base64
import json
import os
from pathlib import Path
import signal

from ndn.app import NDNApp
from ndn.encoding import Name, parse_data
from ndn.security.keychain.keychain_sqlite3 import KeychainSqlite3
from ndn.security.tpm.tpm_file import TpmFile
from ndn.security.validator.known_key_validator import RsaChecker
from ndn.transport.stream_socket import UnixFace
from ndn.types import ValidationFailure, InterestNack, InterestTimeout

from runtime.baseline import write_json


def certificate(role):
    return base64.b64decode(Path("/config", role + ".cert").read_bytes())


def application():
    home = Path.home() / ".ndn"
    keys = KeychainSqlite3(str(home / "pib.db"), TpmFile(str(home / "ndnsec-key-file")))
    return NDNApp(face=UnixFace("/node/nfd.sock"), keychain=keys), keys


async def produce(app, keys, namespace, role):
    identity = namespace + "/" + role
    cert = certificate(role)
    cert_name, _, _, _ = parse_data(cert)
    signer = keys.get_signer({"identity": identity})
    def receive(name, param, payload):
        uri = Name.to_str(name)
        if Name.is_prefix(name, cert_name):
            app.put_raw_packet(cert)
        elif uri.startswith(identity + "/data/") or uri.startswith(identity + "/bad/"):
            content = ("DATA:" + role + ":" + namespace.rsplit("/", 1)[1]).encode()
            packet = app.prepare_data(name, content=content, freshness_period=0, signer=signer)
            if uri.startswith(identity + "/bad/"):
                packet = bytearray(packet)
                packet[-1] ^= 1
            app.put_raw_packet(packet)
    if not await app.register(identity, receive):
        raise RuntimeError("PREFIX_REGISTRATION_REJECTED")
    if role == "raw0":
        root = certificate("root")
        root_name, _, _, _ = parse_data(root)
        def root_request(name, _param, _payload):
            if Name.is_prefix(name, root_name):
                app.put_raw_packet(root)
        if not await app.register(namespace + "/KEY", root_request):
            raise RuntimeError("ROOT_CERTIFICATE_REGISTRATION_REJECTED")
    write_json(Path("/output/ready.json"), {"identity": identity, "certificate": Name.to_str(cert_name)})


async def consume(app, namespace, peer, timeout_ms):
    identity = namespace + "/" + peer
    root_validator = RsaChecker.from_cert(certificate("root"))
    name, _, key_bits, packet = await app.express_interest(
        identity + "/KEY", can_be_prefix=True, must_be_fresh=True,
        lifetime=timeout_ms, validator=root_validator, need_raw_packet=True)
    if not Name.is_prefix(Name.from_str(identity + "/KEY"), name):
        raise RuntimeError("CERTIFICATE_IDENTITY_MISMATCH")
    validator = RsaChecker.from_cert(packet)
    token = namespace.rsplit("/", 1)[1]
    expected = ("DATA:" + peer + ":" + token).encode()
    query = identity + "/data/" + token
    data_name, _, content = await app.express_interest(
        query, lifetime=timeout_ms, must_be_fresh=True, validator=validator)
    if Name.to_str(data_name) != query or bytes(content) != expected:
        raise RuntimeError("DATA_ORACLE_MISMATCH")
    cases = {"raw-roundtrip": {"status": "PASS", "name": query,
                              "payload": bytes(content).decode(), "certificate": Name.to_str(name)}}
    for case, target, can_prefix, checker in [
        ("bad-signature", identity + "/bad/" + token, False, validator),
        ("wrong-root", identity + "/KEY", True, RsaChecker.from_cert(certificate("wrong-root"))),
    ]:
        try:
            await app.express_interest(target, can_be_prefix=can_prefix,
                                       lifetime=timeout_ms, must_be_fresh=True, validator=checker)
        except ValidationFailure:
            cases[case] = {"status": "PASS", "reason": "ValidationFailure"}
        else:
            raise RuntimeError("INVALID_SIGNATURE_ACCEPTED:" + case)
    write_json(Path("/output/result.json"), {"cases": cases})


async def ready_controller(app, namespace, timeout_ms):
    """Probe the old controller's actual authority Face, not start() alone."""
    query = namespace + "/controller/PUBPARAMS/probe/" + namespace.rsplit("/", 1)[1]
    deadline = asyncio.get_running_loop().time() + timeout_ms / 1000
    attempt = 0
    while True:
        try:
            name, _, content = await app.express_interest(
                query + "/" + str(attempt), can_be_prefix=True, must_be_fresh=True,
                lifetime=1000, validator=RsaChecker.from_cert(certificate("controller")))
            break
        except (InterestNack, InterestTimeout):
            if asyncio.get_running_loop().time() >= deadline:
                raise RuntimeError("CONTROLLER_READINESS_TIMEOUT")
            attempt += 1
            await asyncio.sleep(0.1)
    if not content or not Name.is_prefix(Name.from_str(query), name):
        raise RuntimeError("CONTROLLER_PUBPARAMS_INVALID")
    write_json(Path("/output/result.json"), {"status": "PASS", "name": Name.to_str(name)})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["producer", "consumer", "controller-ready"])
    parser.add_argument("--namespace", required=True)
    parser.add_argument("--role", required=True)
    parser.add_argument("--timeout-ms", type=int, default=5000)
    args = parser.parse_args()
    app, keys = application()
    failure = []
    async def work():
        loop = asyncio.get_running_loop()
        loop.add_signal_handler(signal.SIGTERM, app.shutdown)
        try:
            if args.mode == "producer":
                await produce(app, keys, args.namespace, args.role)
            elif args.mode == "consumer":
                await consume(app, args.namespace, args.role, args.timeout_ms)
            else:
                await ready_controller(app, args.namespace, args.timeout_ms)
        except BaseException as exc:
            failure.append(type(exc).__name__ + ":" + str(exc))
        finally:
            if args.mode != "producer" or failure:
                app.shutdown()
    app.run_forever(after_start=work())
    if failure:
        raise RuntimeError(failure[0])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
