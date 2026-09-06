from __future__ import annotations

import ast
import base64
import hashlib
import json
from pathlib import Path
import re

from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from ndnsf_distributed_inference.app_sdk.application import ApplicationDefinitionSigner


ROOT = Path(__file__).resolve().parents[2]
USER = ROOT / "examples/python/NDNSF-DistributedInference/yolo_2x2/user.py"
PROVIDER = ROOT / "examples/python/NDNSF-DistributedInference/yolo_2x2/provider.py"
CONTROLLER = ROOT / "examples/python/NDNSF-DistributedInference/yolo_2x2/controller.py"


def _source() -> str:
    return USER.read_text(encoding="utf-8")


def test_yolo_example_has_model_first_ack_driven_entrypoint():
    source = _source()
    tree = ast.parse(source)
    names = {
        node.name for node in tree.body if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
    }
    assert "_load_yolo_ack_driven" in names
    assert "--offline-oracle" in source
    assert "client.request_task(" in source
    assert "client.publish_application_input_reference(" in source
    ack_helper = source.split("def _load_yolo_ack_driven", 1)[1].split(
        "\ndef main", 1)[0]
    assert "_prepare_yolo_input(args, package)" in ack_helper
    assert "make_input(" not in ack_helper
    assert "_record_yolo_numerical_result(" in ack_helper
    assert "client.encode_input(" not in ack_helper
    assert "--input-payload-file" in source
    assert "ack_coverage_roles=()," in source
    assert 'parser.add_argument("--ack-timeout-ms", type=int, default=1500)' in source
    # The dependency fetch budget follows the request's deadline budget.
    assert "data_v1_no_progress_ms=int(args.timeout_ms)," in ack_helper
    assert "NetworkCatalogSnapshotResolver(" in source
    assert "service_user.fetch_signed_app_data" in source
    assert "LifecycleJournal" in source
    assert "lifecycle_observer=_make_lifecycle_observer" in source
    assert "journal.validate_complete()" in source
    assert 'parser.add_argument("--envelope-key-file", required=True' in source
    assert "envelope_key_file=args.envelope_key_file" in source


def test_legacy_service_policy_calls_are_gated_by_explicit_offline_flag():
    source = _source()
    tree = ast.parse(source)
    main = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "main")
    # The maintained main entrypoint may retain the compatibility implementation,
    # but it must reach it only after the explicit offline-oracle branch.
    branch_lines = [
        node.lineno for node in ast.walk(main)
        if isinstance(node, ast.If)
        and isinstance(node.test, ast.UnaryOp)
        and isinstance(node.test.op, ast.Not)
        and isinstance(node.test.operand, ast.Attribute)
        and node.test.operand.attr == "offline_oracle"
    ]
    assert branch_lines, "main must gate the legacy path with --offline-oracle"
    assert "if not args.offline_oracle:" in source


def test_ack_driven_helper_does_not_accept_provider_or_role_authority():
    source = _source()
    helper = source.split("def _load_yolo_ack_driven", 1)[1].split("\ndef main", 1)[0]
    assert "Provider list" in helper
    assert "role map" in helper
    assert "ack_coverage_roles=()," in helper
    assert "client.request_task(" in helper
    assert "client.publish_application_input_reference(" in helper
    assert "input_reference_file" in helper
    assert "_prepare_yolo_input(args, package)" in helper
    assert "client.distributed_inference(" not in helper
    assert "client.async_distributed_inference(" not in helper
    assert "PreSplitCatalogSnapshot(" not in helper
    assert "catalog_snapshot_file" not in helper


def test_yolo_provider_uses_request_input_and_terminal_owner_apis():
    """The maintained handler must cross the V3 ownership boundary."""
    source = PROVIDER.read_text(encoding="utf-8")
    tree = ast.parse(source)
    handler = next(
        node for node in tree.body
        if isinstance(node, ast.FunctionDef) and node.name == "handle_role")
    calls = {
        node.attr for node in ast.walk(handler)
        if isinstance(node, ast.Attribute)
    }
    assert "_fetch_application_input" in {
        node.func.id for node in ast.walk(handler)
        if isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
    }
    assert "ctx.fetch_application_input(" in source
    assert "ctx.publish_terminal_result(" in source
    assert "fetch_encrypted_large_data" not in {
        node.attr for node in ast.walk(handler)
        if isinstance(node, ast.Attribute)
    }
    assert "_fetch_application_input" in source


def test_native_provider_binds_application_input_only_to_selected_ingress():
    source = (ROOT / "NDNSF-DistributedInference/cpp/ndnsf-di/"
              "NativeProviderHandler.cpp").read_text(encoding="utf-8")
    placement_source = (ROOT / "NDNSF-DistributedInference/"
                        "ndnsf_distributed_inference/app_sdk/placement.py")\
        .read_text(encoding="utf-8")
    assert 'edge.operationKind == "APPLICATION_INPUT"' in source
    assert 'if redistributions else "PIPELINE"' in placement_source
    assert "initialInputs[edge.scope]" in source
    assert "V3 input-ingress role is missing its application input" in source
    assert "DI_INPUT_FETCH_ROLE_MISMATCH" in source
    assert "spec180YnMutation == \"Y-N-I\"" in source


def test_native_protected_binding_excludes_application_input_producer():
    source = (ROOT / "NDNSF-DistributedInference/cpp/ndnsf-di/"
              "NativeProviderHandler.cpp").read_text(encoding="utf-8")
    assert 'endpoint.sourceKind == "APPLICATION_INPUT"' in source
    assert 'endpoint.operation == "APPLICATION_INPUT"' in source
    assert "empty producer binding" in source


def test_yolo_provider_binds_candidate_offer_signing_and_local_onnx_inputs():
    source = PROVIDER.read_text(encoding="utf-8")
    assert "DIProviderOfferIssuerV3" in source
    assert "--selection-offer-key-file" in source
    assert "--local-model-path" in source
    assert "selection_offer_issuer_v3=offer_issuer_v3" in source
    assert "backends=[args.backend]" in source
    assert 'choices=("onnxruntime-cpu", "onnxruntime-cuda")' in source
    assert "dynamic_provisioning = False" in source
    assert "devices=()," in source
    assert 'devices=("cpu:0",)' not in source


def test_yolo_provider_offer_key_loads_with_supported_cryptography_backend(tmp_path: Path):
    source = PROVIDER.read_text(encoding="utf-8")
    tree = ast.parse(source)
    function = next(node for node in tree.body
                    if isinstance(node, ast.FunctionDef)
                    and node.name == "_load_v3_offer_signer")
    namespace = {
        "Path": Path,
        "serialization": serialization,
        "default_backend": default_backend,
        "Ed25519PrivateKey": Ed25519PrivateKey,
        "base64": base64,
        "hashlib": hashlib,
        "RuntimeError": RuntimeError,
    }
    exec(compile(ast.Module([function], type_ignores=[]), str(PROVIDER), "exec"),
         namespace)
    private = Ed25519PrivateKey.generate()
    key_path = tmp_path / "offer.pem"
    key_path.write_bytes(private.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    ))
    key_id, sign = namespace["_load_v3_offer_signer"](str(key_path))
    assert key_id.startswith("sha256:")
    private.public_key().verify(base64.b64decode(sign("sha256:test")),
                                b"sha256:test")


def test_application_definition_signing_key_can_be_reloaded(tmp_path: Path):
    first = ApplicationDefinitionSigner.load_or_create("/example/app", tmp_path)
    second = ApplicationDefinitionSigner.load_or_create("/example/app", tmp_path)
    assert second.key_id == first.key_id
    assert second.public_key == first.public_key


def _spec180_decode_publication():
    """Compile the controller publication decoder into an isolated namespace."""
    source = CONTROLLER.read_text(encoding="utf-8")
    tree = ast.parse(source)
    function = next(node for node in tree.body
                    if isinstance(node, ast.FunctionDef)
                    and node.name == "_decode_spec180_publication")
    namespace = {
        "base64": base64,
        "hashlib": hashlib,
        "re": re,
        "_SPEC180_CASES": {"Y-A", "Y-B", "Y-N"},
        "_SPEC180_DIGEST_RE": re.compile(r"^sha256:[0-9a-f]{64}$"),
        "RuntimeError": RuntimeError,
    }
    exec(compile(ast.Module([function], type_ignores=[]),
                 str(CONTROLLER), "exec"), namespace)
    return namespace["_decode_spec180_publication"]


def test_spec180_runtime_publication_user_keeps_serving_after_readback(
        tmp_path: Path):
    """The published catalogue APP Data must stay servable for the whole case.

    Regression: the publication ServiceUser was stopped right after its own
    readback, which closed its face and dropped the InMemoryStorage entries.
    The User's post-ACK_CLOSED exact-name fetch then timed out even though
    the controller had written a publication receipt.
    """
    source = CONTROLLER.read_text(encoding="utf-8")
    tree = ast.parse(source)
    function = next(node for node in tree.body
                    if isinstance(node, ast.FunctionDef)
                    and node.name == "_publish_spec180_runtime")

    class FakeDeployment:
        controller = "/example/controller"
        group = "/example/group"
        trust_schema = "/tmp/fake-trust-schema"

    class FakeDeploymentLoader:
        def from_config(self, *args, **kwargs):
            return self

    FakeDeploymentLoader.deployment = FakeDeployment()

    published = {}

    class FakeResult:
        success = True
        data_name = ""
        error = ""
        payload = b""
        signer_certificate = "/example/controller"

    class FakeUser:
        last = None

        def __init__(self, **kwargs):
            self.stopped = False
            self.freshness = []
            FakeUser.last = self

        def start(self):
            pass

        def stop(self):
            self.stopped = True

        def publish_signed_app_data(self, name, payload, *, freshness_ms=60000):
            self.freshness.append((str(name), int(freshness_ms)))
            published[str(name)] = bytes(payload)
            result = FakeResult()
            result.data_name = str(name)
            return result

        def fetch_signed_app_data(self, name, signer, *, timeout_ms=5000):
            result = FakeResult()
            result.data_name = str(name)
            result.payload = published[str(name)]
            return result

    decode = _spec180_decode_publication()
    catalogue = b"catalogue"
    artifact = b"artifact"
    digest = lambda value: "sha256:" + hashlib.sha256(value).hexdigest()
    document = {
        "schema": "spec180-runtime-publication-v1",
        "case": "Y-A",
        "catalogueDataName": "/example/controller/NDNSF/DI/catalogue/v1",
        "catalogueSigner": "/example/controller",
        "cataloguePayloadB64": base64.b64encode(catalogue).decode(),
        "cataloguePayloadDigest": digest(catalogue),
        "packageManifestSha256": digest(b"manifest"),
        "artifacts": [{
            "dataName": "/example/controller/NDNSF/DI/ARTIFACT/a/FullModel",
            "payloadB64": base64.b64encode(artifact).decode(),
            "payloadDigest": digest(artifact),
        }],
    }
    publication_path = tmp_path / "runtime-publication.json"
    publication_path.write_text(json.dumps(document), encoding="utf-8")

    namespace = {
        "json": json,
        "Path": Path,
        "hashlib": hashlib,
        "APPDeployment": FakeDeploymentLoader(),
        "ServiceUser": FakeUser,
        "_decode_spec180_publication": decode,
        "RuntimeError": RuntimeError,
    }
    exec(compile(ast.Module([function], type_ignores=[]),
                 str(CONTROLLER), "exec"), namespace)
    publish = namespace["_publish_spec180_runtime"]

    kept = publish("fake-config", "/tmp/fake-policy", str(publication_path))

    assert kept is FakeUser.last, (
        "the publisher must return the serving ServiceUser so main() can keep it alive")
    assert not FakeUser.last.stopped, (
        "stopping the publication ServiceUser drops the signed APP Data")
    assert all(fresh >= 300000 for _, fresh in FakeUser.last.freshness), (
        "catalogue/artifact freshness must cover the full case budget")
    receipt_path = publication_path.with_name("runtime-publication-receipt.json")
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    assert receipt["catalogueDataName"] == "/example/controller/NDNSF/DI/catalogue/v1"
    assert receipt["cataloguePayloadDigest"] == digest(catalogue)


def test_spec180_v3_offer_issuer_disables_simple_service_mirror():
    """A candidate-bound V3 ACK path must not take the simple-service mirror.

    Regression: a single-role, dependency-free service was registered as a
    simple service, so the Provider handler ran with a SimpleResponseContext
    that has no V3 dataflow ownership.  ``fetch_application_input`` then
    failed and the terminal response was fabricated from the raw request.
    """
    from ndnsf_distributed_inference.app_sdk.facades import APPProvider

    captured = {}

    class StubProvider:
        def add_capability_handler(self, service, roles, handler, **kwargs):
            captured.update(kwargs)
            captured["service"] = service
            captured["roles"] = list(roles)

    class StubPolicy:
        artifacts = []
        dependencies = []

    class StubDeployment:
        def service_policy(self, service):
            return StubPolicy()

        def dependency_graph_for_service(self, service):
            return None

        def require_executable_artifacts_allowed(self):
            pass

    app = APPProvider.__new__(APPProvider)
    app.deployment = StubDeployment()
    app._provider = StubProvider()
    app.serve_service(
        service="/AI/YOLO/YOLO26n", roles=["FullModel"], handler=lambda ctx: None,
        backends=["onnxruntime-cpu"], has_model=True, can_provision=False,
        local_artifacts={"FullModel": {"path": "/candidate/model.onnx"}},
        selection_offer_issuer_v3=object(),
    )
    assert captured["service"] == "/AI/YOLO/YOLO26n"
    assert captured["roles"] == ["FullModel"]
    assert captured["register_simple_service"] is False


def test_spec180_provider_prepared_roles_omit_external_artifact_fetch():
    """A preparation-accepted role must not carry an external fetch reference.

    Regression: the sealed plan pointed the C++ collaboration layer at the
    signed ARTIFACT APP Data metadata record as if it were an encrypted
    large-data object.  The Provider holds the validated canonical artifact
    locally, so the plan binds the canonical identity but leaves the fetch
    reference empty; the C++ side then skips the external artifact fetch.
    """
    from ndnsf_distributed_inference.app_sdk import placement as placement_mod
    source = Path(placement_mod.__file__).read_text(encoding="utf-8")
    request_v3 = source.split("def _request_v3", 1)[1].split("\n    def ", 1)[0]
    assert "ACCEPT_IF_EXACT_REUSE" in request_v3
    assert "selected_offer.preparation_accepted" in request_v3
    assert 'fetch_name = ""' in request_v3
    assert "artifact_fetch_names[role] = fetch_name" in request_v3


def test_spec180_controller_enters_publication_mode_without_repo_manifest():
    """The runtime publication batch must not be gated by repo deployment."""
    source = CONTROLLER.read_text(encoding="utf-8")
    assert "not args.spec180_runtime_publication_file" in source
    assert "SPEC180_RUNTIME_CATALOGUE_PUBLISHED" in source
    assert "repository deployment manifest is required" in source


def test_spec180_publication_decoder_rejects_tampered_batch():
    """The controller must validate the cross-process publication envelope."""
    source = CONTROLLER.read_text(encoding="utf-8")
    tree = ast.parse(source)
    function = next(node for node in tree.body
                    if isinstance(node, ast.FunctionDef)
                    and node.name == "_decode_spec180_publication")
    namespace = {
        "base64": base64,
        "hashlib": hashlib,
        "re": re,
        "_SPEC180_CASES": {"Y-A", "Y-B", "Y-N"},
        "_SPEC180_DIGEST_RE": re.compile(r"^sha256:[0-9a-f]{64}$"),
        "RuntimeError": RuntimeError,
    }
    exec(compile(ast.Module([function], type_ignores=[]),
                 str(CONTROLLER), "exec"), namespace)
    decode = namespace["_decode_spec180_publication"]
    catalogue = b"catalogue"
    artifact = b"artifact"
    digest = lambda value: "sha256:" + hashlib.sha256(value).hexdigest()
    document = {
        "schema": "spec180-runtime-publication-v1",
        "case": "Y-A",
        "catalogueDataName": "/example/controller/NDNSF/DI/catalogue/v1",
        "catalogueSigner": "/example/controller",
        "cataloguePayloadB64": base64.b64encode(catalogue).decode(),
        "cataloguePayloadDigest": digest(catalogue),
        "packageManifestSha256": digest(b"manifest"),
        "artifacts": [{
            "dataName": "/example/controller/NDNSF/DI/ARTIFACT/a/FullModel",
            "payloadB64": base64.b64encode(artifact).decode(),
            "payloadDigest": digest(artifact),
        }],
    }
    assert decode(document)[0].endswith("catalogue/v1")
    tampered = dict(document)
    tampered["artifacts"] = [dict(document["artifacts"][0],
                                    payloadDigest=digest(b"changed"))]
    try:
        decode(tampered)
    except RuntimeError as exc:
        assert "payload digest mismatch" in str(exc)
    else:  # pragma: no cover - makes a silent security regression visible
        raise AssertionError("tampered artifact payload was accepted")


def test_controller_background_start_has_native_readiness_barrier():
    """Dependent clients must not race NAC-ABE AA prefix registration.

    The NAC-ABE AttributeAuthority installs its ``PUBPARAMS`` filter from an
    asynchronous register-prefix callback.  A background Controller therefore
    needs an explicit native readiness wait before the publication ServiceUser
    is constructed; a Python thread-start marker alone is insufficient.
    """
    source = (ROOT / "pythonWrapper/ndnsf/service.py").read_text(encoding="utf-8")
    tree = ast.parse(source)
    controller = next(
        node for node in tree.body
        if isinstance(node, ast.ClassDef) and node.name == "ServiceController")
    method = next(
        node for node in controller.body
        if isinstance(node, ast.FunctionDef) and node.name == "start_background")
    calls = [
        node for node in ast.walk(method)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
    ]
    assert any(
        node.func.attr == "wait_until_ready"
        and isinstance(node.func.value, ast.Attribute)
        and node.func.value.attr == "_native"
        for node in calls
    )
