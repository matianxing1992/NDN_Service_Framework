"""Verify the migrated Core direct-input API and installed Python binding.

Historical opt-in DI plan/facade extensions are outside CD-09 and remain in the
protected L0 archive. Native DI is checked through its maintained C++ and
installed MiniNDN deployment paths.
"""

from __future__ import annotations

import inspect
from pathlib import Path
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]

from ndnsf import (  # noqa: E402
    AdmittedInputSet,
    CollaborationInputSpec,
    DataProductReference,
    DependencyRedundancy,
    DependencyTransportOptions,
    VerifiedInputObject,
)
from ndnsf import _ndnsf  # noqa: E402
import ndnsf.service as service_module  # noqa: E402
from ndnsf.service import (  # noqa: E402
    CollaborationRole,
    CollaborationInvocation,
    ServiceUser,
    _dependency_transport_kwargs,
    _role_to_dict,
)


class RecordingNative:
    def __init__(self):
        self.calls = []

    def request_collaboration(self, *args, **kwargs):
        self.calls.append(("request_collaboration", args, kwargs))
        return object()

    def request_collaboration_async(self, *args, **kwargs):
        self.calls.append(("request_collaboration_async", args, kwargs))

    def commit_collaboration_plan(self, *args, **kwargs):
        self.calls.append(("commit_collaboration_plan", args, kwargs))
        return True


class Spec226CompatibilityBindingsTest(unittest.TestCase):
    def test_collaboration_handler_registration_signature_is_unchanged(self):
        self.assertEqual(
            list(inspect.signature(
                service_module.ServiceProvider.add_collaboration_handler
            ).parameters),
            ["self", "service", "allowed_roles", "handler", "ack_handler",
             "include_ack_context"],
        )

    def test_direct_input_types_are_exported_from_package(self):
        self.assertIs(CollaborationInputSpec, service_module.CollaborationInputSpec)
        self.assertIs(AdmittedInputSet, service_module.AdmittedInputSet)
        self.assertIs(VerifiedInputObject, service_module.VerifiedInputObject)

    def test_input_spec_preserves_exact_reference_and_bounds(self):
        reference = DataProductReference(
            object_name="/source/NDNSF/INPUT/v=1",
            producer_name="/source",
            object_class="sensor-frame",
            content_type="application/octet-stream",
            size_bytes=1,
            digest="sha256:frame",
            ttl_ms=5000,
        )
        spec = CollaborationInputSpec(
            slot_id="left",
            reference=reference,
            producer_role="/source",
            key_scope="frame-input",
            max_bytes=4096,
            max_packets=1,
            encoding="signed_exact_packets",
            segment_count=1,
        )

        native = spec._to_native()
        self.assertEqual(native["slot_id"], "left")
        self.assertEqual(native["producer_role"], "/source")
        self.assertEqual(native["key_scope"], "frame-input")
        self.assertEqual(native["max_bytes"], 4096)
        self.assertEqual(native["max_packets"], 1)
        self.assertEqual(native["segment_count"], 1)
        self.assertEqual(native["encoding"], "signed_exact_packets")
        self.assertEqual(native["digest_kind"], "ordered_wire_sha256")
        self.assertEqual(native["reference"]["object_name"], reference.object_name)
        self.assertEqual(native["reference"]["producer_name"], reference.producer_name)
        self.assertEqual(native["reference"]["freshness_ms"], 5000)

    def test_require_inputs_passes_an_immutable_slot_map_to_validator(self):
        marker = object()

        class NativeContext:
            def __init__(self):
                self.specs = None
                self.validator_result = None

            def require_inputs(self, specs, validator):
                self.specs = specs
                self.validator_result = validator({"left": marker})
                return "native-admitted-set"

        class InputView:
            def __init__(self, native):
                self.native = native

        context = service_module.CollaborationContext.__new__(
            service_module.CollaborationContext)
        context._native = NativeContext()
        spec = CollaborationInputSpec(
            slot_id="left",
            reference=DataProductReference(
                object_name="/source/item", producer_name="/source"),
            producer_role="/source",
            key_scope="input",
            max_bytes=4096,
            max_packets=1,
        )
        observed = {}

        def validate(by_slot):
            observed["value"] = by_slot["left"].native
            with self.assertRaises(TypeError):
                by_slot["right"] = object()
            return None

        with mock.patch.object(service_module, "VerifiedInputObject", InputView), \
                mock.patch.object(
                    service_module, "AdmittedInputSet", side_effect=lambda value: value):
            result = context.require_inputs([spec], validate)

        self.assertEqual(result, "native-admitted-set")
        self.assertIs(observed["value"], marker)
        self.assertIs(context._native.validator_result, True)
        self.assertEqual(context._native.specs[0]["slot_id"], "left")

    def test_require_inputs_rejects_invalid_validator_result(self):
        class NativeContext:
            def require_inputs(self, _specs, validator):
                return validator({})

        context = service_module.CollaborationContext.__new__(
            service_module.CollaborationContext)
        context._native = NativeContext()
        with mock.patch.object(service_module, "AdmittedInputSet"):
            with self.assertRaisesRegex(TypeError, "None, True or False"):
                context.require_inputs([], lambda _inputs: "yes")

    def test_final_failure_response_maps_to_existing_native_method(self):
        context = service_module.CollaborationContext.__new__(
            service_module.CollaborationContext)

        class NativeContext:
            def __init__(self):
                self.call = None

            def publish_final_failure_response(self, payload, error_info):
                self.call = (payload, error_info)

        context._native = NativeContext()
        context.publish_final_failure_response(bytearray(b"detail"), "INPUT_TRANSFER_FAILED")
        self.assertEqual(
            context._native.call, (b"detail", "INPUT_TRANSFER_FAILED"))

    def test_input_admission_profile_is_explicit_and_forwarded(self):
        role = CollaborationRole(
            role="/compute", input_admission_profile="data-admission-v1")
        self.assertEqual(
            _role_to_dict(role)["input_admission_profile"],
            "data-admission-v1",
        )
        self.assertEqual(
            _role_to_dict({
                "role": "/compute",
                "input_admission_profile": "data-admission-v1",
            })["input_admission_profile"],
            "data-admission-v1",
        )

    def test_input_admission_profile_rejects_unknown_or_ignored_spelling(self):
        with self.assertRaisesRegex(ValueError, "unsupported input admission"):
            _role_to_dict({"role": "/compute", "input_admission_profile": "future-v9"})
        with self.assertRaisesRegex(TypeError, "use input_admission_profile"):
            _role_to_dict({"role": "/compute", "inputAdmissionProfile": "data-admission-v1"})

    def test_native_options_default_to_none_and_allow_explicit_xor(self):
        options = DependencyTransportOptions()
        self.assertEqual(options.redundancy, DependencyRedundancy.NONE)
        options.redundancy = DependencyRedundancy.XOR
        self.assertEqual(options.redundancy, DependencyRedundancy.XOR)

    def test_old_python_calls_omit_the_additive_keyword(self):
        self.assertEqual(_dependency_transport_kwargs(None), {})

    def test_explicit_transport_is_forwarded_as_the_native_type(self):
        options = DependencyTransportOptions()
        options.redundancy = DependencyRedundancy.XOR
        self.assertEqual(
            _dependency_transport_kwargs(options),
            {"dependency_transport": options},
        )
        with self.assertRaises(TypeError):
            _dependency_transport_kwargs({"redundancy": "xor"})

    def test_public_package_exports_the_native_option_types(self):
        from ndnsf import DependencyRedundancy as exported_redundancy
        from ndnsf import DependencyTransportOptions as exported_options

        self.assertIs(exported_redundancy, _ndnsf.NativeDependencyRedundancy)
        self.assertIs(exported_options, _ndnsf.NativeDependencyTransportOptions)

    def test_old_extension_keeps_legacy_calls_but_rejects_explicit_option(self):
        options = DependencyTransportOptions()
        with mock.patch.object(service_module, "DependencyTransportOptions", None):
            self.assertEqual(_dependency_transport_kwargs(None), {})
            with self.assertRaisesRegex(RuntimeError, "does not support"):
                _dependency_transport_kwargs(options)

    def test_public_sync_and_async_methods_forward_only_explicit_option(self):
        user = ServiceUser.__new__(ServiceUser)
        user._native = RecordingNative()
        base = {
            "roles": [],
            "key_scopes": {},
        }
        with mock.patch.object(service_module, "_from_native_response", return_value="ok"):
            self.assertEqual(user.request_collaboration("/svc", b"", **base), "ok")
        self.assertNotIn("dependency_transport", user._native.calls[-1][2])

        option = DependencyTransportOptions()
        option.redundancy = DependencyRedundancy.XOR
        with mock.patch.object(service_module, "_from_native_response", return_value="ok"):
            user.request_collaboration(
                "/svc", b"", dependency_transport=option, **base)
        self.assertIs(user._native.calls[-1][2]["dependency_transport"], option)

        user.request_collaboration_async(
            "/svc", b"", on_response=lambda _response: None,
            on_timeout=lambda _reason: None, **base)
        self.assertNotIn("dependency_transport", user._native.calls[-1][2])
        user.request_collaboration_async(
            "/svc", b"", on_response=lambda _response: None,
            on_timeout=lambda _reason: None, dependency_transport=option, **base)
        self.assertIs(user._native.calls[-1][2]["dependency_transport"], option)

    def test_deferred_commit_forwards_only_explicit_option(self):
        invocation = CollaborationInvocation.__new__(CollaborationInvocation)
        invocation.acks_closed = mock.Mock(return_value=mock.Mock(digest="closed"))
        invocation._native = RecordingNative()
        invocation.service = "/svc"
        invocation.request_id = "/request/1"
        invocation.ack_timeout_ms = 300
        invocation.timeout_ms = 10000
        args = {
            "ack_closed_digest": "closed",
            "roles": [],
            "key_scopes": {},
        }
        self.assertTrue(invocation.commit_plan(**args))
        self.assertNotIn("dependency_transport", invocation._native.calls[-1][2])

        option = DependencyTransportOptions()
        option.redundancy = DependencyRedundancy.XOR
        self.assertTrue(invocation.commit_plan(
            dependency_transport=option, **args))
        self.assertIs(
            invocation._native.calls[-1][2]["dependency_transport"], option)

    def test_native_request_entrypoints_forward_one_transport_option(self):
        source = (ROOT / "pythonWrapper/src/ndnsf/_ndnsf.cpp").read_text(
            encoding="utf-8")
        self.assertIn("plan.dependencyTransport = dependencyTransport;", source)
        for method in (
            "requestCollaboration(",
            "commitCollaborationPlan(",
            "requestCollaborationAsync(",
        ):
            start = source.index("\n  " + method,
                                 source.index("class NativeServiceUser"))
            end = source.index("\n  }\n", start) + len("\n  }")
            body = source[start:end]
            self.assertIn("dependencyTransport", body, method)
        self.assertEqual(
            source.count('py::arg("dependency_transport") = '
                          "nsf::DependencyTransportOptions{}"),
            3,
        )

    def test_built_native_entrypoints_export_the_optional_argument(self):
        for method in (
            "request_collaboration",
            "commit_collaboration_plan",
            "request_collaboration_async",
        ):
            with self.subTest(method=method):
                self.assertIn(
                    "dependency_transport",
                    getattr(_ndnsf.NativeServiceUser, method).__doc__,
                )


if __name__ == "__main__":
    unittest.main()
