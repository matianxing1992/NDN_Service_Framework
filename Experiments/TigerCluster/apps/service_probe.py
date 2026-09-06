"""Small workload on the SIF's existing native NDNSF service API.

Python supplies payloads and observes callbacks. Native NDNSF owns permissions,
tokens, request publication, ACK validation, Selection, and Response handling.
"""
import argparse
import json
from pathlib import Path
import signal
import threading
import time

from ndnsf import ServiceProvider, ServiceUser
from runtime.baseline import write_json

ECHO = "/TIGER_ECHO"
CONTROL = "/TIGER_CONTROL"


def serve(namespace):
    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    instance = ServiceProvider(provider_prefix=namespace + "/provider",
                                   controller=namespace + "/controller", group=namespace + "/group",
                                   trust_schema="/config/trust.conf", handler_threads=1, ack_threads=1)
    lock = threading.Lock()
    calls = []
    @instance.handler(ECHO)
    def echo(payload):
        with lock:
            calls.append(payload.decode())
            write_json(Path("/output/calls.json"), {"calls": calls[:]})
        return b"ECHO:" + payload
    @instance.handler(CONTROL)
    def control(payload):
        return b"CONTROL:" + payload
    write_json(Path("/output/calls.json"), {"calls": []})
    try:
        instance.start()
        write_json(Path("/output/started.json"), {"role": "provider", "startReturned": True})
        stop.wait()
    finally:
        instance.stop()


def request(namespace, denied, ack_ms, timeout_ms, payload, wrong_trust=False):
    identity = namespace + ("/denied" if denied else "/user")
    user = ServiceUser(user=identity, group=namespace + "/group",
                       controller=namespace + "/controller",
                       trust_schema="/config/trust-wrong.conf" if wrong_trust else "/config/trust.conf",
                       permission_wait_ms=8000, handler_threads=1, ack_threads=1)
    try:
        allowed = sorted({x.service for x in user.get_allowed_services()})
        if wrong_trust:
            if allowed:
                raise RuntimeError("WRONG_NATIVE_TRUST_ACCEPTED")
            write_json(Path("/output/untrusted.json"), {"status": "OBSERVED_EMPTY_PERMISSION",
                                                       "allowed": allowed})
            return
        if denied:
            # A valid granted control service proves that the empty echo
            # permission is not a broken certificate/bootstrap/network path.
            if CONTROL not in allowed or ECHO in allowed:
                raise RuntimeError("NEGATIVE_PERMISSION_PRECONDITION:" + repr(allowed))
            control = user.request_service(CONTROL, b"alive", ack_timeout_ms=ack_ms,
                                           timeout_ms=timeout_ms)
            if not control.status or control.payload != b"CONTROL:alive":
                raise RuntimeError("NEGATIVE_CONTROL_SERVICE_FAILED:" + control.error)
            result = user.request_service(ECHO, b"MUST_NOT_EXECUTE", ack_timeout_ms=ack_ms,
                                          timeout_ms=1000)
            # This old binding waits after Core returns an empty request ID.
            # The worker additionally requires the native permission-rejection
            # log and zero denied Provider execution. A timeout alone is not PASS.
            if result.status or result.request_id not in ("", "/"):
                raise RuntimeError("UNAUTHORIZED_REQUEST_NOT_REJECTED")
            record = {"status": "PASS", "requestId": result.request_id,
                      "error": result.error, "allowed": allowed, "controlPayload": "CONTROL:alive"}
        else:
            if ECHO not in allowed:
                raise RuntimeError("PERMISSION_NOT_READY:" + repr(allowed))
            candidates = []
            selected_providers = []
            def select(offers):
                candidates.extend({"provider": x.provider_name, "status": x.status,
                                   "service": x.service_name, "requestId": x.request_id,
                                   "validated": x.trust_schema_validated,
                                   "signer": x.signer_identity} for x in offers)
                selected = [x.provider_name for x in offers if x.status]
                selected_providers.extend(selected)
                return selected
            result = user.request_service_select(ECHO, payload.encode(), select,
                                                 ack_timeout_ms=ack_ms, timeout_ms=timeout_ms)
            if not result.status or result.payload != ("ECHO:" + payload).encode():
                raise RuntimeError("SERVICE_ORACLE_FAILED:" + result.error)
            if not candidates or not all(c["status"] and
                    c["provider"] == namespace + "/provider" and c["service"] == ECHO and
                    c["requestId"] == result.request_id for c in candidates):
                raise RuntimeError("ACK_EVIDENCE_FAILED:" + repr(candidates))
            record = {"status": "PASS", "payload": result.payload.decode(),
                      "requestId": result.request_id, "dataName": result.data_name,
                      "signerCertificate": result.signer_certificate,
                      "wireDigest": result.wire_digest, "acks": candidates, "allowed": allowed,
                      "selectedProviders": selected_providers,
                      "ackAuthenticationMetadata": "NOT_EXPOSED_BY_GENERIC_V2_BINDING"}
        write_json(Path("/output/result.json"), record)
    finally:
        user.stop()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("role", choices=["provider", "user", "denied", "untrusted"])
    parser.add_argument("--namespace", required=True)
    parser.add_argument("--ack-ms", type=int, default=1500)
    parser.add_argument("--timeout-ms", type=int, default=15000)
    parser.add_argument("--payload", default="hello-tiger")
    args = parser.parse_args()
    if args.role == "provider":
        serve(args.namespace)
    else:
        request(args.namespace, args.role == "denied", args.ack_ms, args.timeout_ms,
                args.payload, wrong_trust=args.role == "untrusted")


if __name__ == "__main__":
    main()
