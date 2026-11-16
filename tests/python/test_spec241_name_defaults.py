"""Bounded source guard; native admission and MiniNDN remain separate evidence."""

import ast
from collections import Counter
from pathlib import Path
import re
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]
ROOTS = ("ndn-service-framework", "NDNSF-DistributedInference", "NDNSF-DistributedRepo",
         "NDNSF-UAV-APP", "pythonWrapper", "Experiments/TigerCluster/runtime")
ENTRY_POINTS = (
    "examples/App_ServiceController.cpp", "examples/DI_NativeProviderExecutable.cpp",
    "examples/DI_NativeRequester.cpp", "examples/DI_NativeArtifactAuthority.cpp",
    "examples/DI_PreparedProvider.cpp",
)
ACTOR = re.compile(r"identity|producer|provider|controller|group|requester|authority|signer|namespace",
                   re.IGNORECASE)
URI = r'''["'](/[^"'\n]+)["']'''
DEFAULTS = (
    re.compile(r"\b(\w+)\s*(?::\s*[\w.\[\], ]+)?\s*=\s*(?:ndn::Name\s*\(\s*)?" + URI),
    re.compile(r"ndn::Name\s+(\w+)\s*[({]\s*" + URI),
    re.compile(r'''py::arg\(\s*["']([^"']+)["']\s*\)\s*=\s*''' + URI),
    re.compile(r'''["']([^"'\n]+)["']\s*,\s*(?:default\s*=\s*)?''' + URI),
)


def candidates(source, suffix):
    """Recognize defaults, named constructor literals and conditional fallbacks, for any root."""
    lines = source.splitlines()
    ignored = set()
    if suffix == ".py":
        tree = ast.parse(source)
        for node in ast.walk(tree):
            body = getattr(node, "body", None)
            if isinstance(body, list) and body and isinstance(body[0], ast.Expr):
                value = body[0].value
                if isinstance(value, ast.Constant) and isinstance(value.value, str):
                    ignored.update(range(body[0].lineno, body[0].end_lineno + 1))
    for index, line in enumerate(lines):
        if index + 1 in ignored or line.lstrip().startswith(("#", "//", "*")):
            continue
        seen = set()
        for pattern in DEFAULTS:
            for match in pattern.finditer(line):
                if ACTOR.search(match[1]):
                    seen.add((match[1], match[2]))
        context = " ".join(lines[max(0, index - 2):index + 1])
        if ACTOR.search(context) and not line.lstrip().startswith(("if ", "elif ")):
            conditional = r"(?:\belse|\bor|\?|:)" if "?" in context else r"(?:\belse|\bor)"
            for match in re.finditer(conditional + r"\s*(?:ndn::Name\s*\(\s*)?" + URI,
                                     line):
                seen.add(("fallback", match[1]))
            if "empty()" in context and "?" in context:
                for match in re.finditer(r"ndn::Name\s*\(\s*" + URI, line):
                    seen.add(("fallback", match[1]))
        for field, uri in sorted(seen):
            yield index + 1, field, uri, line.strip()


# Each exemption is a reviewed symbol/literal, never an entire production file.
# Counts prevent an additional copy of the same exception from silently passing.
LOCAL_REGISTRY = "ndn-service-framework/LocalServiceRegistry.hpp"
UAV_GS = "NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp"
UAV_NAMES = "NDNSF-UAV-APP/shared/UavNames.hpp"
DI_PACKAGE = "NDNSF-DistributedInference/ndnsf_distributed_inference/"
EXCEPTIONS = {
    (LOCAL_REGISTRY, "requesterIdentity", "/local"):
        (4, "LOCAL: in-process registry caller sentinel; network callers supply authenticated identity"),
    (UAV_GS, "requesterIdentity", "/local"):
        (1, "LOCAL: audit helper forwards authenticated identity for network requests"),
    (UAV_GS, "requester_operator", "/example/uav/operator/not-involved"):
        (1, "FIXTURE: explicit negative auto-audit payload; authenticated requester takes precedence"),
    (UAV_NAMES, "SERVICE_GS_OPERATOR_AUTHORITY_LEASE", "/UAV/GS/OperatorAuthority/Lease"):
        (1, "SEMANTIC: operator lease service ID"),
    (UAV_NAMES, "SERVICE_GS_OPERATOR_AUTHORITY_REVOCATION", "/UAV/GS/OperatorAuthority/Revocation"):
        (1, "SEMANTIC: operator revocation service ID"),
    (UAV_NAMES, "SERVICE_GS_OPERATOR_AUTHORITY_AUDIT", "/UAV/GS/OperatorAuthority/Audit"):
        (1, "SEMANTIC: operator audit service ID"),
    ("NDNSF-DistributedRepo/src/backends/FilesystemRepoStoreBackend.cpp",
     "publisherIdentity", "/repo"):
        (1, "LOCAL: private payload-store filesystem reference, never original Data Producer"),
    (DI_PACKAGE + "runtime_v1.py", "provider", "/llm-8gb"):
        (1, "FIXTURE: runtime_v1_smoke in-process cache model"),
    (DI_PACKAGE + "runtime_v1.py", "fallback", "/NDNSF/DI/PROVIDER-LOCAL"):
        (1, "LOCAL: legacy exact-forward cache metadata, not a network publication identity"),
    ("examples/App_ServiceController.cpp", "fallback", "/SERVICE"):
        (1, "PROTOCOL: provider service authorization attribute"),
    ("examples/App_ServiceController.cpp", "fallback", "/PERMISSION"):
        (1, "PROTOCOL: requester service authorization attribute"),
}


def production_files():
    tracked = subprocess.check_output(
        ["git", "ls-files", "--", *ROOTS, *ENTRY_POINTS], cwd=ROOT, text=True).splitlines()
    for relative in tracked:
        path = Path(relative)
        if path.suffix not in (".cpp", ".hpp", ".py"):
            continue
        if relative not in ENTRY_POINTS and {"tests", "examples", "configs"}.intersection(path.parts):
            continue
        # These Python tools are standalone offline/model experiments; no production
        # C++ owner or Waf install target calls them. The installed native probe is included.
        if relative.startswith("NDNSF-UAV-APP/tools/") and path.suffix == ".py":
            continue
        yield relative


class ActorDefaultGuard(unittest.TestCase):
    def test_production_defaults_have_reviewed_semantics(self):
        observed = Counter()
        unexpected = []
        for relative in production_files():
            source = (ROOT / relative).read_text()
            for line, field, uri, text in candidates(source, Path(relative).suffix):
                key = (relative, field, uri)
                observed[key] += 1
                if key not in EXCEPTIONS or observed[key] > EXCEPTIONS[key][0]:
                    unexpected.append(f"{relative}:{line}: {field}={uri}: {text}")
        self.assertEqual(unexpected, [], "\n".join(unexpected))
        self.assertEqual(dict(observed), {key: value[0] for key, value in EXCEPTIONS.items()},
                         "Re-audit stale classification entries rather than retaining unused exemptions")

    def test_arbitrary_namespace_mutations_are_detected(self):
        mutations = (
            ('std::string providerName = "/unseen/site/actor";', ".cpp"),
            ('ndn::Name controllerIdentity{"/brand/new/authority"};', ".hpp"),
            ('const auto identityName = signingIdentity.empty() ?\n'
             '  ndn::Name("/other/root/actor") : ndn::Name(signingIdentity);', ".cpp"),
            ('group: str = "/another/deployment"', ".py"),
            ('controller = config.get("controller", "/independent/root")', ".py"),
            ('provider = configured or "/unexpected/provider"', ".py"),
            ('namespaceRoot = "/new/namespace";', ".cpp"),
            ('py::arg("controller") = "/new/binding/actor"', ".cpp"),
        )
        for source, suffix in mutations:
            with self.subTest(source=source):
                self.assertTrue(list(candidates(source, suffix)))

    def test_protocols_suffixes_and_documented_examples_are_not_actor_defaults(self):
        self.assertFalse(list(candidates('const ndn::Name REQUEST("/NDNSF/REQUEST");', ".hpp")))
        self.assertFalse(list(candidates('name = provider + "/NDNSF/RESPONSE";', ".cpp")))
        self.assertFalse(list(candidates('"""Example: provider=\'/demo/actor\'"""', ".py")))
        # A protocol-looking URI becomes a defect when it supplies a concrete Actor.
        self.assertTrue(list(candidates('provider = "/NDNSF/REQUEST"', ".py")))


if __name__ == "__main__":
    unittest.main()
