"""C08 native wire parity and SDK ownership boundaries; transport IO is a stub.

This suite requires the actual candidate extension. It does not qualify NDN
signature validation or network retrieval, which the native/runtime gates do.
"""
from dataclasses import replace
import hashlib
import importlib.util
import os
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest
from ndn.encoding import Component, Name

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "pythonWrapper/src"))
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
binary = Path(os.environ["SPEC201_PYTHON_BINDING"]).resolve(strict=True)
spec = importlib.util.spec_from_file_location("_ndnsf", binary)
native = importlib.util.module_from_spec(spec)
spec.loader.exec_module(native)
assert Path(native.__file__).resolve() == binary
sys.modules["ndnsf._ndnsf"] = native

from ndnsf_distributed_inference.app_sdk.application import ApplicationDefinitionSigner
from ndnsf_distributed_inference.app_sdk.contracts import (
    DeploymentActivationRecord, ModelIntent, RequestContract,
    OptimizationObjective, DeploymentConstraints,
)
from ndnsf_distributed_inference.app_sdk.deployment import (
    DeploymentCatalog, NetworkDeploymentCatalogTransport,
)

PRODUCER = "/owner/final"
APPLICATION = "/app/creator"
SHA_A = "sha256:" + "a" * 64
SHA_B = "sha256:" + "b" * 64


def wire(name):
    return bytes(Name.encode(Name.normalize(name)))


def deployment_key(application, deployment_id):
    # Independent C01 framing: no production digest or naming helper.
    domain = b"ndnsf-naming/deployment"
    framed = len(domain).to_bytes(2, "big") + domain + b"\x00\x02\x00\x02"
    for tag, kind, value in ((1, 1, wire(application)), (2, 2, deployment_id.encode())):
        framed += tag.to_bytes(2, "big") + bytes([kind]) + len(value).to_bytes(8, "big") + value
    return hashlib.sha256(framed).digest()


@pytest.mark.parametrize("kind,revision,epoch,suffix", [
    ("DEFINITION", "", 0, ["DEFINITION"]),
    ("ACTIVATION", SHA_B, 0, ["REVISION", bytes.fromhex("b" * 64), "ACTIVATION"]),
    ("REVOCATION", SHA_B, 257, ["REVISION", bytes.fromhex("b" * 64), "REVOCATION", "EPOCH", "257"]),
])
def test_actual_native_c08_matches_independent_component_wire(kind, revision, epoch, suffix):
    actual = native.make_deployment_record_name(PRODUCER, APPLICATION, "deployment/one",
                                                kind, revision, epoch, SHA_A)
    expected = Name.normalize(PRODUCER)
    components = ["NDNSF", "DEPLOYMENT", "name=/8=app/8=creator",
                  deployment_key(APPLICATION, "deployment/one"), *suffix, bytes.fromhex("a" * 64)]
    expected += [Component.from_bytes(value if isinstance(value, bytes) else value.encode())
                 for value in components]
    assert wire(actual) == bytes(Name.encode(expected))
    assert wire(actual) == wire(native.make_deployment_record_name(
        "/8=owner/8=final", "/8=app/8=creator", "deployment/one", kind, revision, epoch, SHA_A))


def activation():
    return DeploymentActivationRecord(APPLICATION, PRODUCER, "deployment", SHA_B,
        "/service", SHA_A, SHA_B, SHA_A, 1, "2026-10-09T00:00:00+00:00",
        "2099-01-01T00:00:00+00:00", "key", "signature")


def test_activation_name_avoids_self_reference_without_changing_signed_digest():
    value = activation()
    name = NetworkDeploymentCatalogTransport._activation_name(PRODUCER, value)
    located = replace(value, record_name=name)
    assert located.naming_digest() == value.naming_digest()
    assert located.digest() != value.digest()
    assert located.canonical_dict()["record_name"] == name
    assert NetworkDeploymentCatalogTransport._activation_name(PRODUCER, located) == name
    assert replace(located, lifecycle_epoch=2).naming_digest() != located.naming_digest()
    assert replace(located, signature="different").digest() == located.digest()
    revoked = replace(located, state="REVOKED", revoked=True, lifecycle_epoch=2)
    assert wire(NetworkDeploymentCatalogTransport._activation_name(PRODUCER, revoked)) != wire(name)


class TransportStub:
    """Attested-result stub: tests SDK checks, never claims cryptographic proof."""
    requester_identity = PRODUCER

    def __init__(self):
        self.records = {}
        self.fetches = 0

    def publish_signed_app_data(self, name, payload, **kwargs):
        self.records[wire(name)] = bytes(payload)
        return SimpleNamespace(success=True, data_name=name)

    def fetch_signed_app_data(self, name, signer, **kwargs):
        self.fetches += 1
        return SimpleNamespace(success=True, data_name=name,
            payload=self.records[wire(name)], signer_certificate=signer + "/KEY/key/issuer/v=1")


def signed_definition():
    signer = ApplicationDefinitionSigner.generate(APPLICATION)
    value = signer.define(deployment_id="deployment", deployment_owner=PRODUCER,
        service="/service", model_intent=ModelIntent(("model",)), artifacts=(),
        request_contract=RequestContract("input", "output"),
        objective=OptimizationObjective("latency"),
        constraints=DeploymentConstraints(), optimization_profile="default")
    return signer, value


def test_actual_catalog_publication_and_fetch_require_explicit_owner_and_app_key():
    signer, value = signed_definition()
    user = TransportStub()
    transport = NetworkDeploymentCatalogTransport(user)
    creator = DeploymentCatalog(owner_identity=PRODUCER,
        definition_publisher=transport.publish_definition)
    creator.authorize_application(APPLICATION, signer.key_id, signer.public_key)
    ref = creator.publish_definition(value)
    expected = native.make_deployment_record_name(PRODUCER, APPLICATION,
        value.deployment_id, "DEFINITION", "", 0, value.digest())
    assert wire(ref.record_name) == wire(expected)
    with pytest.raises(ValueError, match="explicit Producer"):
        transport.fetch_definition(ref.record_name, value.digest())
    with pytest.raises(ValueError, match="expected Producer"):
        transport.fetch_definition(ref.record_name, value.digest(), expected_producer="/foreign")
    assert user.fetches == 0
    fetched = transport.fetch_definition(ref.record_name, value.digest(), expected_producer=PRODUCER)
    remote = DeploymentCatalog(owner_identity="/remote", definition_fetcher=transport.fetch_definition)
    with pytest.raises(PermissionError, match="not authorized"):
        remote._admit_fetched_definition(fetched)
    remote.authorize_application(APPLICATION, signer.key_id, signer.public_key)
    assert remote.resolve_definition(ref)[0] == value
    before = len(user.records)
    with pytest.raises(PermissionError, match="lifecycle owner"):
        transport.publish_definition(ref.record_name, replace(value, deployment_owner="/foreign"))
    assert len(user.records) == before


def test_activation_transport_checks_original_record_name_and_owner():
    user = TransportStub()
    transport = NetworkDeploymentCatalogTransport(user)
    value = activation()
    name = transport._activation_name(PRODUCER, value)
    value = replace(value, record_name=name)
    transport.publish_activation(name, value)
    fetched = transport.fetch_activation(name, value.digest(), expected_producer="/8=owner/8=final")
    assert fetched.activation == value
    with pytest.raises(ValueError, match="commitment"):
        transport.publish_activation(name, replace(value, lifecycle_epoch=2))
    with pytest.raises(ValueError, match="explicit Producer"):
        transport.fetch_activation(name, value.digest())
    with pytest.raises(PermissionError, match="lifecycle owner"):
        transport.publish_activation(name, replace(value, deployment_owner="/foreign"))
