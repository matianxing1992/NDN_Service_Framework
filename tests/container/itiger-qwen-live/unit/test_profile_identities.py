from __future__ import annotations

import copy
from pathlib import Path
import sys
import unittest

import yaml


REPO = Path(__file__).resolve().parents[4]
LIB = REPO / "packaging" / "ndnsf-di-container" / "lib"
if str(LIB) not in sys.path:
    sys.path.insert(0, str(LIB))

import profile as profile_impl


class ProfileIdentityTest(unittest.TestCase):
    def setUp(self) -> None:
        source = REPO / "tests/container/fixtures/profiles/itiger-rtx5000-valid.yaml"
        self.profile = yaml.safe_load(source.read_text())
        self.profile["profileId"] = "spec110-single-node"
        root = "/project/tma1/ndnsf-di/identities/spec110-set"
        self.profile["identity"] = {
            "reference": root,
            "readOnly": True,
            "roleReferences": {
                "controller": root + "/controller",
                "user": root + "/user",
                "providers": [root + "/provider-0", root + "/provider-1", root + "/provider-2"],
            },
        }

    def test_distinct_readonly_role_identity_set_is_accepted(self) -> None:
        result = profile_impl.validate_profile(self.profile)
        self.assertTrue(result["identity"]["readOnly"])
        self.assertEqual(3, len(result["identity"]["roleReferences"]["providers"]))

    def test_spec110_profile_requires_role_identities(self) -> None:
        changed = copy.deepcopy(self.profile)
        changed["identity"].pop("roleReferences")
        with self.assertRaisesRegex(profile_impl.ProfileError, "PROFILE_SPEC110_ROLE_IDENTITIES_REQUIRED"):
            profile_impl.validate_profile(changed)

    def test_duplicate_role_identity_is_rejected(self) -> None:
        changed = copy.deepcopy(self.profile)
        changed["identity"]["roleReferences"]["providers"][0] = changed["identity"]["roleReferences"]["user"]
        with self.assertRaisesRegex(profile_impl.ProfileError, "PROFILE_ROLE_IDENTITIES_NOT_DISTINCT"):
            profile_impl.validate_profile(changed)

    def test_role_identity_outside_set_is_rejected(self) -> None:
        changed = copy.deepcopy(self.profile)
        changed["identity"]["roleReferences"]["controller"] = "/project/tma1/ndnsf-di/identities/other/controller"
        with self.assertRaisesRegex(profile_impl.ProfileError, "PROFILE_ROLE_IDENTITY_OUTSIDE_READONLY_SET"):
            profile_impl.validate_profile(changed)

    def test_writable_identity_binding_is_rejected_by_schema(self) -> None:
        changed = copy.deepcopy(self.profile)
        changed["identity"]["readOnly"] = False
        with self.assertRaises(profile_impl.ProfileError):
            profile_impl.validate_profile(changed)


if __name__ == "__main__":
    unittest.main()
