from __future__ import annotations

import unittest

from contract._support import FIXTURES, load_impl

release_impl = load_impl("release")
ReleaseError = release_impl.ReleaseError
load_release_manifest = release_impl.load_release_manifest


class ReleaseManifestTest(unittest.TestCase):
    def test_digest_pinned_release_is_valid(self) -> None:
        value = load_release_manifest(FIXTURES / "releases" / "release-valid.json")
        self.assertEqual(value["releaseId"], "spec108-r1")

    def test_tag_only_and_mismatched_digest_are_rejected(self) -> None:
        for name in ("release-tag-only.json", "release-mismatched-digest.json"):
            with self.subTest(name=name), self.assertRaises(ReleaseError):
                load_release_manifest(FIXTURES / "releases" / name)


if __name__ == "__main__":
    unittest.main()
