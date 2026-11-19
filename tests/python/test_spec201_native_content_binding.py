"""Exercise current source-linked pybind admission without network publication.

The empty service deliberately stops at Core's first publication guard after
the real binding has copied and validated semantic metadata. Packet generation
and decryption are covered by the native publication selectors.
"""
import gc
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from native_service_fixture import provision_native_identities


class NativeContentBinding(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from ndnsf import _ndnsf
        binding = os.environ.get("SPEC201_PYTHON_BINDING")
        if binding and Path(_ndnsf.__file__).resolve() != Path(binding).resolve(strict=True):
            raise AssertionError("use the interpreter that owns the requested NDNSF binding")
        cls.native = _ndnsf
        cls.directory = tempfile.TemporaryDirectory(prefix="spec201-native-binding-")
        cls.addClassCleanup(cls.directory.cleanup)
        environment, trust_schema = provision_native_identities(
            Path(cls.directory.name), "/spec201/binding/controller", "/spec201/binding/user")
        isolate = patch.dict(os.environ, environment)
        isolate.start()
        cls.addClassCleanup(isolate.stop)
        print("SOURCE_BINDING_LOADED", flush=True)
        cls.user = cls.native.NativeServiceUser(
            group="/spec201/binding/group", controller="/spec201/binding/controller",
            user="/spec201/binding/user", trust_schema=str(trust_schema),
            permission_wait_ms=0, handler_threads=1, ack_threads=1,
            serve_certificates=False)
        cls.addClassCleanup(cls.stop_user)
        print("SOURCE_BINDING_USER_READY", flush=True)

    @classmethod
    def stop_user(cls):
        cls.user.stop()
        del cls.user
        gc.collect()

    def test_requester_identity_is_native_owned_and_readonly(self):
        from ndnsf.service import ServiceUser
        facade = ServiceUser.__new__(ServiceUser)
        facade._native = self.user
        facade.user = "/foreign/configuration"
        self.assertEqual(facade.requester_identity,
                         "/8=spec201/8=binding/8=user")
        self.assertEqual(self.user.requester_identity, facade.requester_identity)
        with self.assertRaises(AttributeError):
            self.user.requester_identity = "/foreign/native"
        # The facade owns no new Face/native owner in this configuration check.
        del facade._native

    def test_all_semantic_families_reach_core_guard(self):
        for kind in ("blob", "model-file", "weights", "initializer", "tokenizer",
                     "artifact-file", "material", "input", "assignment"):
            fields = {"kind": kind, "label": "权重/α.dat"}
            if kind in ("model-file", "weights", "initializer", "tokenizer", "artifact-file", "assignment"):
                fields.update(model_name="qwen2.5-0.5b", model_identity="a" * 64)
            if kind == "artifact-file":
                fields.update(artifact_kind="stage", coordinate="layers=0-11",
                              profile_identity="b" * 64)
            if kind == "input":
                fields["input_kind"] = "image"
            with self.subTest(kind=kind):
                original = dict(fields)
                result = self.user.publish_encrypted_large_data(
                    "", b"payload", object_label="binding", content_name=fields)
                self.assertFalse(result.success)
                self.assertEqual(result.error, "PreparedServiceRequest serviceName is empty")
                self.assertEqual(fields, original)

    def test_invalid_metadata_rejected_by_binding(self):
        for fields in ([], {}, {"kind": "unknown", "label": "x"},
                       {"kind": "blob", "label": "x", "producer": "/foreign"},
                       {"kind": "model-file", "label": "x"},
                       {"kind": "blob", "label": "x", "model_identity": "a" * 64},
                       {"kind": "input", "label": "x", "input_kind": "a/b"},
                       {"kind": "assignment", "label": "worker"},
                       {"kind": "assignment", "label": "worker", "model_name": "qwen",
                        "model_identity": "a" * 64, "input_kind": "application"}):
            with self.subTest(fields=fields):
                with self.assertRaises(ValueError):
                    self.user.publish_encrypted_large_data("", b"payload", content_name=fields)

    def test_omitted_metadata_keeps_existing_keyword_defaults(self):
        for _ in range(1000):
            result = self.user.publish_encrypted_large_data("", b"payload", object_label="existing")
            self.assertFalse(result.success)
            self.assertEqual(result.error, "PreparedServiceRequest serviceName is empty")
        del result
        gc.collect()


if __name__ == "__main__":
    unittest.main()
