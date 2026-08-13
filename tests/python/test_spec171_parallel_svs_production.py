import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


class ParallelSvsContractTest(unittest.TestCase):
    def test_parallel_receive_requires_explicit_opt_in(self):
        sources = {
            "user": REPO_ROOT / "ndn-service-framework" / "ServiceUser.cpp",
            "provider": REPO_ROOT / "ndn-service-framework" / "ServiceProvider.cpp",
        }

        opt_in = re.compile(
            r'std::getenv\("NDNSF_SVS_PARALLEL_SYNC"\)\s*!=\s*nullptr\s*&&\s*'
            r'isTruthyEnv\("NDNSF_SVS_PARALLEL_SYNC"\)'
        )
        unsafe_default = re.compile(
            r'std::getenv\("NDNSF_SVS_PARALLEL_SYNC"\)\s*==\s*nullptr\s*\|\|'
        )

        for role, path in sources.items():
            with self.subTest(role=role):
                source = path.read_text(encoding="utf-8")
                self.assertRegex(source, opt_in)
                self.assertNotRegex(source, unsafe_default)
                self.assertIn(
                    f"NDNSF_SVS_PARALLEL_SYNC disabled role={role}",
                    source,
                )

    def test_parallel_production_requires_explicit_opt_in(self):
        sources = {
            "user": REPO_ROOT / "ndn-service-framework" / "ServiceUser.cpp",
            "provider": REPO_ROOT / "ndn-service-framework" / "ServiceProvider.cpp",
        }

        opt_in = re.compile(
            r'std::getenv\("NDNSF_SVS_PARALLEL_PRODUCTION"\)\s*!=\s*nullptr\s*&&\s*'
            r'isTruthyEnv\("NDNSF_SVS_PARALLEL_PRODUCTION"\)'
        )
        unsafe_default = re.compile(
            r'std::getenv\("NDNSF_SVS_PARALLEL_PRODUCTION"\)\s*==\s*nullptr\s*\|\|'
        )

        for role, path in sources.items():
            with self.subTest(role=role):
                source = path.read_text(encoding="utf-8")
                self.assertRegex(source, opt_in)
                self.assertNotRegex(source, unsafe_default)
                self.assertIn(
                    f"NDNSF_SVS_PARALLEL_PRODUCTION disabled role={role}",
                    source,
                )


if __name__ == "__main__":
    unittest.main()
