from __future__ import annotations

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
RUNTIME_SOURCES = (
    ROOT / "ndn-service-framework/ServiceUser.cpp",
    ROOT / "ndn-service-framework/ServiceProvider.cpp",
)


class ReliableSvsPublishContractTest(unittest.TestCase):
    def test_async_control_publish_is_explicit_opt_in(self):
        for source in RUNTIME_SOURCES:
            text = source.read_text(encoding="utf-8")
            match = re.search(
                r"bool\s+useAsyncSvsPublish\(\)\s*\{(?P<body>.*?)\n\s*\}",
                text,
                re.DOTALL,
            )
            self.assertIsNotNone(match, source)
            body = match.group("body")
            self.assertIn('std::getenv("NDNSF_SVS_ASYNC_PUBLISH") != nullptr', body)
            self.assertNotIn('std::getenv("NDNSF_SVS_ASYNC_PUBLISH") == nullptr', body)
            self.assertIn('isTruthyEnv("NDNSF_SVS_ASYNC_PUBLISH")', body)

    def test_synchronous_publish_remains_the_reliable_default_path(self):
        for source in RUNTIME_SOURCES:
            text = source.read_text(encoding="utf-8")
            self.assertRegex(
                text,
                r"if \(useAsyncSvsPublish\(\)\) \{\s*"
                r"svs->publishAsync\(name, content\);\s*"
                r"\}\s*else \{\s*svs->publish\(name, content\);",
            )


if __name__ == "__main__":
    unittest.main()
