"""Exercise the real native binding's fail-before-identity-creation boundary."""

import inspect
import functools
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from ndnsf import ServiceController, ServiceProvider, ServiceUser
from ndnsf import _ndnsf


def isolated_native_case(method):
    """ndn-cxx caches default locators; give each case its own native process."""
    @functools.wraps(method)
    def run(self):
        if os.environ.get("NDNSF_SPEC241_ACTOR_CHILD") == "1":
            return method(self)
        environment = dict(os.environ, NDNSF_SPEC241_ACTOR_CHILD="1")
        result = subprocess.run(
            [sys.executable, str(Path(__file__).resolve()),
             type(self).__name__ + "." + method.__name__],
            env=environment, capture_output=True, text=True, check=False, timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
    return run


class ExplicitActorInputs(unittest.TestCase):
    def setUp(self):
        self.home = (None if os.environ.get("NDNSF_SPEC241_ACTOR_CHILD") == "1" else
                     tempfile.TemporaryDirectory(prefix="ndnsf-actor-inputs-"))
        # The child's environment is already isolated before importing ndn-cxx.
        self.directory = self.home.name if self.home is not None else os.environ["HOME"]
        self.previous_home = os.environ.get("HOME")
        self.previous_certificate = os.environ.pop("NDNSF_CONTROLLER_CERT_FILE", None)
        os.environ["HOME"] = self.directory
        self.previous_locators = {name: os.environ.get(name)
                                  for name in ("NDN_CLIENT_PIB", "NDN_CLIENT_TPM")}
        os.environ["NDN_CLIENT_PIB"] = "pib-sqlite3:" + self.directory + "/pib"
        os.environ["NDN_CLIENT_TPM"] = "tpm-file:" + self.directory + "/tpm"

    def tearDown(self):
        for name, value in self.previous_locators.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value
        if self.previous_home is None:
            os.environ.pop("HOME", None)
        else:
            os.environ["HOME"] = self.previous_home
        if self.previous_certificate is None:
            os.environ.pop("NDNSF_CONTROLLER_CERT_FILE", None)
        else:
            os.environ["NDNSF_CONTROLLER_CERT_FILE"] = self.previous_certificate
        if self.home is not None:
            self.home.cleanup()

    def assert_no_candidate_identity(self, allowed=()):
        result = subprocess.run(["ndnsec", "list"], capture_output=True, text=True,
                                check=False, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("/spec241/candidate", result.stdout)
        self.assertNotIn("/example/hello", result.stdout)
        identities = set(re.findall(
            r"^\s*(?:[├└]─)?[─ *]*(/\S+)\s*$", result.stdout, re.MULTILINE))
        self.assertEqual(identities, set(allowed), result.stdout)

    @isolated_native_case
    def test_missing_actor_is_rejected_by_each_native_role(self):
        for role in (_ndnsf.NativeServiceProvider, _ndnsf.NativeServiceUser,
                     _ndnsf.NativeServiceController):
            with self.subTest(role=role.__name__):
                with self.assertRaisesRegex(ValueError, "explicit non-root NDN name"):
                    role()
        self.assert_no_candidate_identity()

    @isolated_native_case
    def test_root_names_do_not_trigger_identity_creation(self):
        roles = (
            (_ndnsf.NativeServiceProvider,
             dict(group="/spec241/candidate/group", controller="/spec241/candidate/controller",
                  provider_prefix="/spec241/candidate/provider")),
            (_ndnsf.NativeServiceUser,
             dict(group="/spec241/candidate/group", controller="/spec241/candidate/controller",
                  user="/spec241/candidate/requester")),
            (_ndnsf.NativeServiceController,
             dict(controller_prefix="/spec241/candidate/controller")),
        )
        for role, valid in roles:
            for field in valid:
                with self.subTest(role=role.__name__, field=field):
                    arguments = dict(valid, **{field: "/"})
                    with self.assertRaisesRegex(ValueError, field):
                        role(**arguments)
        self.assert_no_candidate_identity()

    @isolated_native_case
    def test_foreign_controller_certificate_is_rejected_before_actor_creation(self):
        certificate = Path(self.directory) / "foreign.cert"
        result = subprocess.run(["ndnsec", "key-gen", "/spec241/foreign/controller"],
                                capture_output=True, check=True, timeout=20)
        certificate.write_bytes(result.stdout)
        os.environ["NDNSF_CONTROLLER_CERT_FILE"] = str(certificate)
        for role, actor in ((_ndnsf.NativeServiceProvider, {"provider_prefix": "/spec241/candidate/provider"}),
                            (_ndnsf.NativeServiceUser, {"user": "/spec241/candidate/requester"})):
            with self.subTest(role=role.__name__):
                with self.assertRaisesRegex(RuntimeError, "does not match controller"):
                    role(group="/spec241/candidate/group",
                         controller="/spec241/candidate/controller", **actor)
        self.assert_no_candidate_identity(("/spec241/foreign/controller",))

    @isolated_native_case
    def test_explicit_provider_keeps_complete_configured_identity(self):
        controller = "/spec241/explicit/controller/root"
        certificate = Path(self.directory) / "controller.cert"
        generated = subprocess.run(["ndnsec", "key-gen", controller],
                                   capture_output=True, check=True, timeout=20)
        certificate.write_bytes(generated.stdout)
        schema = Path(self.directory) / "trust.conf"
        schema.write_text('trust-anchor\n{\n  type file\n  file-name "' +
                          str(certificate) + '"\n}\n')
        os.environ["NDNSF_CONTROLLER_CERT_FILE"] = str(certificate)
        identity = "/spec241/explicit/provider/nested"
        provider = _ndnsf.NativeServiceProvider(
            group="/spec241/explicit/sync/domain", controller=controller,
            provider_prefix="/spec241/explicit/provider", provider_id="nested",
            trust_schema=str(schema), serve_certificates=False,
            handler_threads=0, ack_threads=0,
        )
        try:
            self.assertEqual(provider.provider_identity, identity)
            self.assert_no_candidate_identity((controller, identity))
        finally:
            provider.stop()

    @isolated_native_case
    def test_facade_and_native_defaults_have_no_example_actor(self):
        for role, fields in ((ServiceProvider, ("group", "controller", "provider_prefix")),
                             (ServiceUser, ("group", "controller", "user")),
                             (ServiceController, ("controller_prefix",))):
            parameters = inspect.signature(role).parameters
            for field in fields:
                self.assertEqual(parameters[field].default, "")


if __name__ == "__main__":
    unittest.main()
