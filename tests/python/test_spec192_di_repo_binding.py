from __future__ import annotations

from dataclasses import asdict, fields
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace

import pytest

from ndnsf_distributed_inference.app_sdk.contracts import (
    ArtifactReference as DIArtifactReference,
    DIRepoArtifactBinding,
)
from ndnsf_distributed_inference.app_sdk.facades import APPClient, APPProvider
from ndnsf_distributed_inference.client import _plan_fingerprint
from ndnsf_distributed_inference.artifact_deployment import (
    ExecutionArtifact,
    ExecutionArtifactSpec,
    materialize_role_artifacts,
    publish_execution_artifact_spec,
    prepare_execution,
    _write_cached_artifact,
)
from ndnsf_distributed_inference.plan import ArtifactSpec
from ndnsf_distributed_inference.repo_artifact_resolver import (
    DIRepoArtifactResolver,
    binding_from_native_handle,
)
from py_repoclient import ArtifactReference as NativeArtifactReference
from py_repoclient import ArtifactRepositoryApi


def _native_inapp_artifact(tmp_path: Path, payload: bytes):
    source = tmp_path / "source.bin"
    source.write_bytes(payload)
    digest = hashlib.sha256(payload).hexdigest()
    api = ArtifactRepositoryApi.make_in_app(
        durable_root=tmp_path / "repo",
        owner_id="spec192-di-test",
    )
    reference = NativeArtifactReference(
        logical_name="/Spec192/DI/model.bin",
        content_digest=digest,
        size_bytes=len(payload),
        root_manifest_name="/Spec192/DI/model.bin/root",
        publisher_identity="/spec192/test-publisher",
        policy_epoch="/Policy/spec192/v1",
    )
    result = api.publish_file(source, reference)
    assert result.handle is not None
    assert result.receipt is not None
    assert result.receipt.state == "COMMITTED"
    return api, result.handle, digest


def _di_reference(digest: str, size: int, *, uri: str = "urn:spec192:model"):
    return DIArtifactReference(
        uri=uri,
        digest="sha256:" + digest,
        size_bytes=size,
        mount_path="model.bin",
    )


def _instrument_native_api(monkeypatch, api):
    calls = {"fetches": 0, "publications": 0}
    native_fetch = api.fetch_file

    def fetch_file(handle, destination, timeout_ms=30000):
        calls["fetches"] += 1
        return native_fetch(handle, destination, timeout_ms=timeout_ms)

    def no_publication(*_args, **_kwargs):
        calls["publications"] += 1
        raise AssertionError("DI resolution must not publish or archive Repo artifacts")

    monkeypatch.setattr(api, "fetch_file", fetch_file)
    monkeypatch.setattr(api, "publish_file", no_publication)
    monkeypatch.setattr(api, "archive", no_publication)
    return calls


def test_binding_projects_native_handle_for_model_input_and_output(tmp_path):
    payload = b"spec192 native DI artifact"
    _api, handle, digest = _native_inapp_artifact(tmp_path, payload)
    generic = _di_reference(digest, len(payload))

    for use in ("model", "input", "output"):
        binding = binding_from_native_handle(
            handle, generic, role="Detect", use=use)
        decoded = DIRepoArtifactBinding.from_dict(binding.to_dict())
        assert decoded == binding
        assert decoded.artifact_reference == generic
        assert decoded.repo_handle_json == handle.to_json()

    # The existing generic DI value keeps its original public schema.
    assert [item.name for item in fields(DIArtifactReference)] == [
        "uri", "digest", "size_bytes", "mount_path"
    ]


def test_service_plan_transports_repo_model_input_and_output_bindings(tmp_path):
    payload = b"Spec192 role-scoped network artifact"
    _native_api, handle, digest = _native_inapp_artifact(tmp_path, payload)
    local_model = tmp_path / "local-model.bin"
    local_model.write_bytes(b"service-policy model remains local")
    bindings = {
        slot: binding_from_native_handle(
            handle,
            _di_reference(digest, len(payload), uri=f"urn:spec192:{slot}"),
            role="Detect",
            use=slot,
        )
        for slot in ("model", "input", "output")
    }

    class Deployment:
        def service_policy(self, service):
            assert service == "/Spec192/DI/Infer"
            return SimpleNamespace(
                name=service,
                model_name="spec192-role-model",
                dependencies=[],
                artifacts=[SimpleNamespace(
                    role="Detect",
                    artifact_name="/Spec192/DI/model",
                    backend="onnxruntime",
                    path=str(local_model),
                    filename="model.bin",
                    kind="onnx-model",
                    metadata={},
                )],
            )

    client = APPClient.__new__(APPClient)
    client.deployment = Deployment()
    client._service_runtimes = {}
    plan = client.service_plan(
        "/Spec192/DI/Infer",
        artifact_references={"roles": {"Detect": {
            slot: {
                "repoBinding": binding.to_dict(),
                "filename": f"{slot}.bin",
                "kind": "onnx-model" if slot == "model" else "application-data",
            }
            for slot, binding in bindings.items()
        }}},
    )

    encoded = plan.roles[0].artifacts()
    assert {slot for slot in encoded} == {"model", "input", "output"}
    for slot, binding in bindings.items():
        assert encoded[slot]["repoBinding"] == binding.to_dict()
        assert encoded[slot]["repo_binding"] == binding.to_dict()
        assert encoded[slot]["payload"] == b""

    alternate_input = binding_from_native_handle(
        handle,
        _di_reference(digest, len(payload), uri="urn:spec192:alternate-input"),
        role="Detect",
        use="input",
    )
    alternate_plan = client.service_plan(
        "/Spec192/DI/Infer",
        artifact_references={"roles": {"Detect": {
            "model": {"repoBinding": bindings["model"].to_dict()},
            "input": {"repoBinding": alternate_input.to_dict()},
            "output": {"repoBinding": bindings["output"].to_dict()},
        }}},
    )
    assert _plan_fingerprint(alternate_plan) != _plan_fingerprint(plan)

    input_only_plan = client.service_plan(
        "/Spec192/DI/Infer",
        artifact_references={"roles": {"Detect": {
            "input": {"repoBinding": bindings["input"].to_dict()},
        }}},
    )
    assert input_only_plan.roles[0].model_artifact.payload == local_model.read_bytes()

    wrong_scope = dict(bindings["input"].to_dict())
    wrong_scope["use"] = "output"
    with pytest.raises(ValueError, match="use does not match"):
        client.service_plan(
            "/Spec192/DI/Infer",
            artifact_references={"roles": {"Detect": {
                "model": {"repoBinding": bindings["model"].to_dict()},
                "input": {"repoBinding": wrong_scope},
            }}},
        )


def test_generic_di_artifact_reference_local_path_is_unchanged(tmp_path):
    payload = b"generic non-Repo artifact"
    source = tmp_path / "generic.bin"
    source.write_bytes(payload)
    reference = {
        "uri": "urn:application:generic-model:v1",
        "digest": "sha256:" + hashlib.sha256(payload).hexdigest(),
        "size_bytes": len(payload),
        "mount_path": "model.bin",
    }

    artifacts = materialize_role_artifacts(
        {"roles": {"Detect": {
            "model": {
                "artifactReference": reference,
                "localPayloadPath": str(source),
            }
        }}},
        "Detect",
        tmp_path / "cache",
    )

    model = artifacts["model"]
    assert model.path.read_bytes() == payload
    assert model.artifact_reference == reference
    assert model.repo_binding is None


def test_binding_rejects_digest_size_and_schema_mismatch(tmp_path):
    payload = b"native reference identity"
    _api, handle, digest = _native_inapp_artifact(tmp_path, payload)
    with pytest.raises(ValueError, match="digest/size"):
        binding_from_native_handle(
            handle,
            _di_reference(digest, len(payload) + 1),
            role="Detect",
            use="model",
        )
    wrong_digest = ("0" if not digest.startswith("0") else "1") + digest[1:]
    with pytest.raises(ValueError, match="digest/size"):
        binding_from_native_handle(
            handle,
            _di_reference(wrong_digest, len(payload)),
            role="Detect",
            use="model",
        )

    binding = binding_from_native_handle(
        handle, _di_reference(digest, len(payload)), role="Detect", use="model")
    raw = binding.to_dict()
    raw["schemaVersion"] = 2
    with pytest.raises(ValueError, match="schema"):
        DIRepoArtifactBinding.from_dict(raw)


def test_resolver_uses_native_fetch_and_never_publishes(tmp_path, monkeypatch):
    payload = b"authorized native fetch"
    native_api, handle, digest = _native_inapp_artifact(tmp_path, payload)
    binding = binding_from_native_handle(
        handle, _di_reference(digest, len(payload)), role="Detect", use="model")

    calls = _instrument_native_api(monkeypatch, native_api)
    resolver = DIRepoArtifactResolver(native_api)
    assert resolver.fetch_bytes(binding) == payload
    assert calls == {"fetches": 1, "publications": 0}
    with pytest.raises(TypeError, match="exact T008"):
        DIRepoArtifactResolver(object())


def test_repo_resolution_rejects_caller_owned_python_adapters(tmp_path):
    class PythonResolver:
        def fetch_bytes(self, _binding):
            return b"unverified bytes"

    with pytest.raises(TypeError, match="DIRepoArtifactResolver"):
        materialize_role_artifacts(
            {}, "Detect", tmp_path / "cache", repo_resolver=PythonResolver())
    with pytest.raises(TypeError, match="DIRepoArtifactResolver"):
        prepare_execution(None, repo_resolver=PythonResolver())


def test_execution_spec_carries_opaque_binding_without_repo_manifest(tmp_path):
    payload = b"spec transported over collaboration"
    _native_api, handle, digest = _native_inapp_artifact(tmp_path, payload)
    binding = binding_from_native_handle(
        handle, _di_reference(digest, len(payload)), role="Detect", use="model")

    class CaptureUser:
        captured = None

        def publish_encrypted_large_data(self, _service, wire, **_options):
            self.captured = bytes(wire)
            return SimpleNamespace(success=True, encrypted_data_name="/spec192/spec")

    user = CaptureUser()
    result = publish_execution_artifact_spec(
        user,
        "/Spec192/DI/Infer",
        role="Detect",
        backend="onnxruntime",
        artifacts={"model": {
            "repoBinding": binding.to_dict(),
            "filename": "model.bin",
            "kind": "onnx-model",
        }},
    )
    assert result.success
    published = ExecutionArtifactSpec.from_bytes(user.captured)
    assert published.artifacts[0].repo_binding == binding.to_dict()
    assert published.artifacts[0].repo_manifest == {}
    assert published.artifacts[0].sha256 == digest

    user.captured = None
    with pytest.raises(ValueError, match="conflicting Repo binding aliases"):
        publish_execution_artifact_spec(
            user,
            "/Spec192/DI/Infer",
            role="Detect",
            backend="onnxruntime",
            artifacts={"model": {
                "repoBinding": {},
                "repo_binding": binding.to_dict(),
                "payload": b"must not fall through to generic publication",
            }},
        )
    assert user.captured is None


def test_warm_di_cache_does_not_bypass_repo_authorization(tmp_path, monkeypatch):
    payload = b"cached bytes still require native authorization"
    native_api, handle, digest = _native_inapp_artifact(tmp_path, payload)
    calls = _instrument_native_api(monkeypatch, native_api)

    def denied_fetch(*_args, **_kwargs):
        calls["fetches"] += 1
        raise PermissionError("Repo authorization denied")

    monkeypatch.setattr(native_api, "fetch_file", denied_fetch)
    binding = binding_from_native_handle(
        handle, _di_reference(digest, len(payload)), role="Detect", use="model")
    artifact = ExecutionArtifact(
        name="model",
        data_name="",
        filename="model.bin",
        sha256=digest,
        cache_name="spec192-di-warm-cache",
        repo_binding=binding.to_dict(),
    )
    cache_root = tmp_path / "cache"
    monkeypatch.setenv("NDNSF_ARTIFACT_CACHE_DIR", str(cache_root))
    _write_cached_artifact(artifact, payload)

    spec = ExecutionArtifactSpec(
        role="Detect",
        backend="test",
        artifacts=[artifact],
    )
    serialized = json.loads(spec.to_bytes().decode("utf-8"))
    serialized_artifact = serialized["artifacts"][0]
    serialized_artifact["repo_binding"] = serialized_artifact.pop("repoBinding")
    snake_case_wire = json.dumps(serialized, sort_keys=True).encode("utf-8")
    assert ExecutionArtifactSpec.from_bytes(snake_case_wire).artifacts[0].repo_binding == binding.to_dict()

    class Context:
        assignment = SimpleNamespace(
            assigned_artifact="/spec192/execution",
            role="Detect",
            service="/Spec192/DI/Infer",
            provisioning_timeout_ms=1000,
        )

        def fetch_artifact(self, _name, _timeout):
            return True

        def get_artifact(self, _name):
            return snake_case_wire

    resolver = DIRepoArtifactResolver(native_api)
    with pytest.raises(PermissionError, match="authorization denied"):
        prepare_execution(Context(), temp_root=tmp_path / "work", repo_resolver=resolver)
    assert calls["fetches"] == 1

    conflicting_wire = json.loads(spec.to_bytes().decode("utf-8"))
    conflicting_artifact = conflicting_wire["artifacts"][0]
    conflicting_artifact["repoBinding"] = {}
    conflicting_artifact["repo_binding"] = binding.to_dict()
    conflicting_wire_bytes = json.dumps(conflicting_wire, sort_keys=True).encode("utf-8")

    class ConflictingContext(Context):
        def get_artifact(self, _name):
            return conflicting_wire_bytes

    with pytest.raises(ValueError, match="conflicting Repo binding aliases"):
        prepare_execution(
            ConflictingContext(),
            temp_root=tmp_path / "conflicting-work",
            repo_resolver=resolver,
        )
    assert calls["fetches"] == 1


def test_execution_spec_role_must_match_assignment_before_repo_fetch(tmp_path, monkeypatch):
    payload = b"role-bound spec artifact"
    native_api, handle, digest = _native_inapp_artifact(tmp_path, payload)
    calls = _instrument_native_api(monkeypatch, native_api)
    binding = binding_from_native_handle(
        handle,
        _di_reference(digest, len(payload)),
        role="OtherRole",
        use="model",
    )
    spec = ExecutionArtifactSpec(
        role="OtherRole",
        backend="test",
        artifacts=[ExecutionArtifact(
            name="model",
            data_name="",
            filename="model.bin",
            sha256=digest,
            repo_binding=binding.to_dict(),
        )],
    )

    class Context:
        assignment = SimpleNamespace(
            assigned_artifact="/spec192/execution",
            role="Detect",
            service="/Spec192/DI/Infer",
            provisioning_timeout_ms=1000,
        )

        def fetch_artifact(self, _name, _timeout):
            return True

        def get_artifact(self, _name):
            return spec.to_bytes()

    resolver = DIRepoArtifactResolver(native_api)
    with pytest.raises(RuntimeError, match="role does not match the assignment"):
        prepare_execution(Context(), temp_root=tmp_path / "work", repo_resolver=resolver)
    assert calls["fetches"] == 0


def test_role_materialization_fetches_again_when_cache_is_warm(tmp_path, monkeypatch):
    payload = b"role model and retained input"
    native_api, handle, digest = _native_inapp_artifact(tmp_path, payload)
    calls = _instrument_native_api(monkeypatch, native_api)
    model = binding_from_native_handle(
        handle,
        _di_reference(digest, len(payload), uri="urn:spec192:model"),
        role="Detect",
        use="model",
    )
    retained_input = binding_from_native_handle(
        handle,
        _di_reference(digest, len(payload), uri="urn:spec192:input"),
        role="Detect",
        use="input",
    )
    retained_output = binding_from_native_handle(
        handle,
        _di_reference(digest, len(payload), uri="urn:spec192:output"),
        role="Detect",
        use="output",
    )
    references = {"roles": {"Detect": {
        "model": {"repoBinding": model.to_dict(), "filename": "model.bin"},
        "input": {"repoBinding": retained_input.to_dict(), "filename": "input.bin"},
        "output": {"repoBinding": retained_output.to_dict(), "filename": "output.bin"},
    }}}

    resolver = DIRepoArtifactResolver(native_api)
    cache = tmp_path / "di-cache"
    artifacts = materialize_role_artifacts(
        references, "Detect", cache, repo_resolver=resolver)
    assert set(artifacts) == {"model", "input", "output"}
    assert all(item.path.read_bytes() == payload for item in artifacts.values())
    assert calls["fetches"] == 3

    def denied_fetch(*_args, **_kwargs):
        calls["fetches"] += 1
        raise PermissionError("Repo authorization denied")

    monkeypatch.setattr(native_api, "fetch_file", denied_fetch)

    with pytest.raises(PermissionError, match="authorization denied"):
        materialize_role_artifacts(
            references, "Detect", cache, repo_resolver=resolver)
    assert calls["fetches"] == 4


def test_role_and_use_binding_scope_is_checked_before_fetch(tmp_path, monkeypatch):
    payload = b"scoped native role artifact"
    native_api, handle, digest = _native_inapp_artifact(tmp_path, payload)
    calls = _instrument_native_api(monkeypatch, native_api)
    resolver = DIRepoArtifactResolver(native_api)
    wrong_role = binding_from_native_handle(
        handle,
        _di_reference(digest, len(payload)),
        role="OtherRole",
        use="model",
    )
    with pytest.raises(ValueError, match="role does not match"):
        materialize_role_artifacts(
            {"roles": {"Detect": {
                "model": {"repoBinding": wrong_role.to_dict()}
            }}},
            "Detect",
            tmp_path / "cache",
            repo_resolver=resolver,
        )
    assert calls["fetches"] == 0
    wrong_use = binding_from_native_handle(
        handle,
        _di_reference(digest, len(payload)),
        role="Detect",
        use="input",
    )
    with pytest.raises(ValueError, match="use does not match"):
        materialize_role_artifacts(
            {"roles": {"Detect": {
                "model": {"repoBinding": wrong_use.to_dict()}
            }}},
            "Detect",
            tmp_path / "cache",
            repo_resolver=resolver,
        )
    assert calls["fetches"] == 0


def test_repo_binding_does_not_mask_retired_manifest_fields(tmp_path, monkeypatch):
    monkeypatch.setenv("NDNSF_ARTIFACT_CACHE_DIR", str(tmp_path / "execution-cache"))
    payload = b"binding cannot mask the old Repo manifest path"
    native_api, handle, digest = _native_inapp_artifact(tmp_path, payload)
    resolver = DIRepoArtifactResolver(native_api)
    binding = binding_from_native_handle(
        handle,
        _di_reference(digest, len(payload)),
        role="Detect",
        use="model",
    )
    entries = [
        {"repoBinding": binding.to_dict(), "repoManifest": {"root": "/old"}},
        {"repoBinding": binding.to_dict(), "largeDataReference": {
            "source": "repo-manifest", "manifestDigest": "sha256:old",
        }},
        {"repoBinding": binding.to_dict(), "largeDataReference": {
            "sourceType": "repo_manifest", "manifestDigest": "sha256:old",
        }},
        {"repoBinding": binding.to_dict(), "largeDataReference": {},
         "large_data_reference": {"source": "repo-manifest"}},
        {"repoBinding": binding.to_dict(), "largeDataReference": {
            "source": "generic", "sourceType": "repo-manifest",
        }},
        {"repoBinding": binding.to_dict(), "source": "repo-manifest"},
        {"repoBinding": binding.to_dict(), "source": "generic",
         "sourceType": "repo_manifest"},
    ]
    for entry in entries:
        with pytest.raises(RuntimeError, match="SPEC192_COMPATIBILITY_V1"):
            APPClient._repo_binding_for_entry(entry)
        with pytest.raises(RuntimeError, match="SPEC192_COMPATIBILITY_V1"):
            materialize_role_artifacts(
                {"roles": {"Detect": {"model": entry}}},
                "Detect",
                tmp_path / "cache",
                repo_resolver=resolver,
            )

    artifact = ExecutionArtifact(
        name="model",
        data_name="",
        filename="model.bin",
        sha256=digest,
        repo_manifest={"root": "/old"},
        repo_binding=binding.to_dict(),
    )
    spec = ExecutionArtifactSpec(role="Detect", backend="test", artifacts=[artifact])

    hidden_legacy_wire = json.loads(spec.to_bytes().decode("utf-8"))
    hidden_legacy_artifact = hidden_legacy_wire["artifacts"][0]
    hidden_legacy_artifact.pop("repoManifest")
    hidden_legacy_artifact["repo_manifest"] = {"root": "/old"}
    hidden_legacy_artifact.pop("largeDataReference")
    hidden_legacy_artifact["large_data_reference"] = {
        "sourceType": "repo_manifest",
        "manifestDigest": "sha256:old",
    }
    with pytest.raises(RuntimeError, match="SPEC192_COMPATIBILITY_V1"):
        ExecutionArtifactSpec.from_bytes(
            json.dumps(hidden_legacy_wire, sort_keys=True).encode("utf-8"))

    class Context:
        assignment = SimpleNamespace(
            assigned_artifact="/spec192/execution",
            role="Detect",
            service="/Spec192/DI/Infer",
            provisioning_timeout_ms=1000,
        )

        def fetch_artifact(self, _name, _timeout):
            return True

        def get_artifact(self, _name):
            return spec.to_bytes()

    with pytest.raises(RuntimeError, match="SPEC192_COMPATIBILITY_V1"):
        prepare_execution(Context(), temp_root=tmp_path / "work", repo_resolver=resolver)

    cached = ExecutionArtifact(
        name="model",
        data_name="",
        filename="model.bin",
        sha256=digest,
        cache_name="spec192-retired-manifest",
        repo_manifest={"root": "/old"},
    )
    _write_cached_artifact(cached, payload)
    spec_without_binding = ExecutionArtifactSpec(
        role="Detect", backend="test", artifacts=[cached])

    class CachedContext(Context):
        def get_artifact(self, _name):
            return spec_without_binding.to_bytes()

    with pytest.raises(RuntimeError, match="SPEC192_COMPATIBILITY_V1"):
        prepare_execution(
            CachedContext(), temp_root=tmp_path / "cached-work", repo_resolver=resolver)

    plan_artifact = ArtifactSpec(
        name="model",
        payload=b"",
        filename="model.bin",
        repo_binding=binding.to_dict(),
        large_data_reference={"source": "repo-manifest"},
    )
    with pytest.raises(RuntimeError, match="SPEC192_COMPATIBILITY_V1"):
        plan_artifact.to_ndnsf_artifact()


def test_conflicting_repo_binding_aliases_fail_closed(tmp_path, monkeypatch):
    monkeypatch.setenv("NDNSF_ARTIFACT_CACHE_DIR", str(tmp_path / "execution-cache"))
    payload = b"conflicting aliases cannot select a generic local path"
    native_api, handle, digest = _native_inapp_artifact(tmp_path, payload)
    resolver = DIRepoArtifactResolver(native_api)
    binding = binding_from_native_handle(
        handle,
        _di_reference(digest, len(payload)),
        role="Detect",
        use="model",
    ).to_dict()
    entry = {
        "repoBinding": {},
        "repo_binding": binding,
        "artifactReference": asdict(_di_reference(digest, len(payload))),
        "localPayloadPath": str(tmp_path / "generic-fallback.bin"),
    }
    (tmp_path / "generic-fallback.bin").write_bytes(payload)

    with pytest.raises(ValueError, match="conflicting Repo binding aliases"):
        APPClient._repo_binding_for_entry(entry)
    with pytest.raises(ValueError, match="conflicting Repo binding aliases"):
        materialize_role_artifacts(
            {"roles": {"Detect": {"model": entry}}},
            "Detect",
            tmp_path / "cache",
            repo_resolver=resolver,
        )

    empty_generic = {
        "repoBinding": {},
        "repo_binding": {},
        "artifactReference": asdict(_di_reference(digest, len(payload))),
        "localPayloadPath": str(tmp_path / "generic-fallback.bin"),
    }
    assert APPClient._repo_binding_for_entry(empty_generic) == {}
    materialized = materialize_role_artifacts(
        {"roles": {"Detect": {"model": empty_generic}}},
        "Detect",
        tmp_path / "empty-alias-cache",
        repo_resolver=resolver,
    )
    assert materialized["model"].path.read_bytes() == payload


def test_repo_resolver_uses_collaboration_registration_path():
    class ProviderCapture:
        def add_capability_handler(self, *_args, **kwargs):
            self.kwargs = kwargs

    class Deployment:
        def service_policy(self, _service):
            return SimpleNamespace(artifacts=[], dependencies=[])

        def dependency_graph_for_service(self, _service):
            return None

    def registered_kwargs(repo_resolver):
        app = object.__new__(APPProvider)
        app._provider = ProviderCapture()
        app.deployment = Deployment()
        app.serve_service(
            service="/Spec192/DI/Infer",
            roles=["Inspect"],
            handler=lambda _ctx: None,
            repo_resolver=repo_resolver,
        )
        return app._provider.kwargs

    resolver = object()
    repo_kwargs = registered_kwargs(resolver)
    assert repo_kwargs["repo_resolver"] is resolver
    assert repo_kwargs["register_simple_service"] is False

    local_kwargs = registered_kwargs(None)
    assert local_kwargs["register_simple_service"] is True
