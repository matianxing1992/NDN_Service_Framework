"""spec181 T008 unit layer: the Y-B protected-epoch grant seam.

Builds the requester-side in-process authority seam exactly as user.py does
under SPEC181_PROTECTION_EPOCH, drives one ProviderGrantViewV1 through it,
and unwraps the published grant with the Provider's recipient key (the same
production verify_and_unwrap_grant call the Provider makes).  No NFD, no
process boundary.
"""
import importlib.util, sys, json, os, tempfile
from pathlib import Path

sys.path.insert(0, "/home/tianxing/NDN/ndn-service-framework")
sys.path.insert(0, "/home/tianxing/NDN/ndn-service-framework/NDNSF-DistributedRepo/pythonWrapper")
sys.path.insert(0, "/home/tianxing/NDN/ndn-service-framework/NDNSF-DistributedInference")
sys.path.insert(0, "/home/tianxing/NDN/ndn-service-framework/pythonWrapper")
sys.path.insert(0, "/home/tianxing/NDN/ndn-service-framework/examples/python/NDNSF-DistributedInference/yolo_2x2")

# Generate a temp authority key + recipient map, then drive _build_grant_seam
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519

tmp = tempfile.mkdtemp()
authority_key = ed25519.Ed25519PrivateKey.generate()
recipient_key = ed25519.Ed25519PrivateKey.generate()
authority_path = Path(tmp) / "artifact-policy-authority.key"
authority_path.write_bytes(authority_key.private_bytes(
    serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
    serialization.NoEncryption()))
os.chmod(authority_path, 0o600)
recipient_path = Path(tmp) / "recipient.pem"
recipient_path.write_bytes(recipient_key.private_bytes(
    serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
    serialization.NoEncryption()))
recipient_map = {"/example/provider/BackboneNeck": str(recipient_path)}
map_path = Path(tmp) / "map.json"
map_path.write_text(json.dumps(recipient_map))
requester_path = Path(tmp) / "requester.key"
requester_path.write_bytes(os.urandom(32))

os.environ["SPEC181_PROTECTION_EPOCH"] = "spec180-yolo-protected-v1"
os.environ["SPEC181_REQUESTER_PRIVATE_KEY"] = str(requester_path)
os.environ["SPEC181_PROVIDER_RECIPIENT_KEY_MAP"] = str(map_path)
os.environ["NDNSF_SPEC180_CONFIG_ROOT"] = str(tmp)

# load user.py module
spec = importlib.util.spec_from_file_location(
    "yolo_user", "/home/tianxing/NDN/ndn-service-framework/examples/python/NDNSF-DistributedInference/yolo_2x2/user.py")
user_mod = importlib.util.module_from_spec(spec)
sys.modules["yolo_user"] = user_mod
spec.loader.exec_module(user_mod)

class FakeServiceUser:
    def __init__(self):
        self.published = []
    def publish_signed_app_data(self, name, payload, freshness_ms=60000):
        self.published.append((name, payload))

class FakeNetworkClient:
    service_user = FakeServiceUser()

class FakeClient:
    _network_client = FakeNetworkClient()

provider, epoch = user_mod._build_grant_seam(FakeClient())
assert provider is not None, "seam must build under a protected epoch"
assert epoch == "spec180-yolo-protected-v1"

# drive a grant view through the seam
from ndnsf_distributed_inference.sdk.placement import ProviderGrantViewV1
view = ProviderGrantViewV1(
    provider="/example/provider/BackboneNeck", request_id="req-yb-1", attempt=1,
    plan_core_digest="sha256:" + "cd" * 32,
    offer_digest="sha256:" + "ef" * 32,
    role_digests=("sha256:" + "ab" * 32,),
    security_policy_snapshot_digest="sha256:" + "88" * 32,
    model_manifest_digest="sha256:" + "11" * 32,
    protection_epoch="spec180-yolo-protected-v1",
)
import time
binding = provider(view, deadline_ms=int(time.time() * 1000) + 60000)
assert binding.grant_name.startswith("/example/user/NDNSF-DI/KEY-GRANT/v1")
assert len(FakeNetworkClient.service_user.published) == 1
published_name, wire = FakeNetworkClient.service_user.published[0]
assert published_name == binding.grant_name

# Provider side: unwrap with the recipient key (same production call)
from ndnsf_distributed_inference.core.protected_artifacts import (
    grant_from_wire, verify_and_unwrap_grant)
grant = grant_from_wire(wire)
content_key = verify_and_unwrap_grant(
    grant, authority_public_key=authority_key.public_key(),
    recipient_private_key=recipient_key,
    expected_provider_identity="/example/provider/BackboneNeck",
    expected_request_id="req-yb-1", expected_attempt=1,
    expected_plan_core_digest="sha256:" + "cd" * 32,
    expected_model_manifest_digest="sha256:" + "11" * 32,
    expected_protection_epoch="spec180-yolo-protected-v1",
    now_ms=int(time.time() * 1000),
)
assert len(content_key) == 32
def test_y_b_grant_seam_round_trip():
    pass  # the module-level assertions above ARE the test body


print("YB_SEAM_UNIT_OK")
