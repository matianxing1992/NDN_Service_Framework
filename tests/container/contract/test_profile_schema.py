from __future__ import annotations

import unittest

from contract._support import FIXTURES, load_impl

profile_impl = load_impl("profile")
ProfileError = profile_impl.ProfileError
load_profile = profile_impl.load_profile


class ProfileSchemaTest(unittest.TestCase):
    def test_valid_profiles(self) -> None:
        for name in ("cloud-cpu-valid.yaml", "itiger-rtx5000-valid.yaml"):
            with self.subTest(name=name):
                profile = load_profile(FIXTURES / "profiles" / name)
                self.assertIn(profile["runtime"]["kind"], {"docker-compose", "slurm-apptainer"})

    def test_invalid_profiles_fail_closed(self) -> None:
        for path in sorted((FIXTURES / "profiles" / "invalid").glob("*.yaml")):
            with self.subTest(path=path.name), self.assertRaises(ProfileError):
                load_profile(path)


if __name__ == "__main__":
    unittest.main()
