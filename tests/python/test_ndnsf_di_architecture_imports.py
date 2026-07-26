from __future__ import annotations

import ast
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
PKG = ROOT / "NDNSF-DistributedInference" / "ndnsf_distributed_inference"
FIXTURE = ROOT / "tests/fixtures/ndnsf-di-core-app-separation/ownership-matrix.json"
SNAPSHOT = ROOT / "tests/python/support/ndnsf_di_import_snapshot.py"


def imports_from(path: Path) -> set[str]:
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    imports: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            imports.update(alias.name for alias in node.names)
        elif isinstance(node, ast.ImportFrom):
            module = node.module or ""
            if node.level:
                package = ".".join(path.relative_to(PKG).with_suffix("").parts[:-1])
                imports.add("ndnsf_distributed_inference." + ".".join(filter(None, (package, module))))
            elif module:
                imports.add(module)
    return imports


def forbidden_imports(path: Path, denied: list[str]) -> set[str]:
    return {
        imported for imported in imports_from(path)
        if any(imported == owner or imported.startswith(owner + ".") for owner in denied)
    }


class ArchitectureImportsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.matrix = json.loads(FIXTURE.read_text(encoding="utf-8"))

    def test_fixture_is_bound_to_normative_contract(self) -> None:
        self.assertEqual(self.matrix["schema"], "ndnsf-di-spec111-ownership-matrix-v1")
        self.assertTrue((ROOT / self.matrix["source"]).is_file())
        self.assertEqual(
            set(self.matrix["phases"]),
            {"legacy", "core", "sdk", "planner", "requestScopedAssignment"},
        )

    def test_progressive_owner_import_gate(self) -> None:
        phase = os.environ.get("NDNSF_DI_ARCH_PHASE", "legacy")
        gate = self.matrix["phases"][phase]
        for root_module in gate["roots"]:
            module_path = PKG.joinpath(*root_module.split(".")[1:])
            files = [module_path.with_suffix(".py")] if module_path.with_suffix(".py").is_file() else list(module_path.rglob("*.py"))
            self.assertTrue(files, f"missing phase root {root_module}")
            for path in files:
                for imported in imports_from(path):
                    self.assertFalse(
                        any(imported == denied or imported.startswith(denied + ".") for denied in gate["forbiddenImports"]),
                        f"{phase}: {path.relative_to(ROOT)} imports forbidden owner {imported}",
                    )

    def test_optional_imports_are_absent_from_core_snapshot_when_core_exists(self) -> None:
        core = PKG / "core" / "__init__.py"
        if not core.is_file():
            self.skipTest("Core extraction starts in Phase 3")
        gate = self.matrix["phases"]["core"]
        command = [sys.executable, str(SNAPSHOT), "ndnsf_distributed_inference.core"]
        for denied in gate["forbiddenImports"]:
            command += ["--blocked", denied]
        env = dict(os.environ)
        env["PYTHONPATH"] = str(ROOT / "NDNSF-DistributedInference") + os.pathsep + env.get("PYTHONPATH", "")
        completed = subprocess.run(command, cwd=ROOT, env=env, text=True, capture_output=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertEqual(json.loads(completed.stdout)["forbiddenLoaded"], [])

    def test_core_gate_detects_a_forbidden_owner_import(self) -> None:
        fixture = ROOT / "tests/fixtures/ndnsf-di-core-app-separation/forbidden-core-import.py"
        fixture.write_text(
            "from ndnsf_distributed_inference.app import APPClient\n",
            encoding="utf-8")
        self.addCleanup(fixture.unlink, missing_ok=True)
        denied = self.matrix["phases"]["core"]["forbiddenImports"]
        self.assertIn("ndnsf_distributed_inference.app", forbidden_imports(fixture, denied))

    def test_global_role_provider_preference_bridge_is_absent(self) -> None:
        forbidden = "NDNSF_COLLAB_ROLE_PROVIDER_PREFERENCE"
        production_roots = (
            ROOT / "NDNSF-DistributedInference",
            ROOT / "pythonWrapper",
            ROOT / "examples/python/NDNSF-DistributedInference",
        )
        matches = []
        for root in production_roots:
            for path in root.rglob("*"):
                if path.is_file() and path.suffix in {".py", ".cpp", ".hpp", ".h"}:
                    if forbidden in path.read_text(encoding="utf-8", errors="ignore"):
                        matches.append(str(path.relative_to(ROOT)))
        self.assertEqual(matches, [])


if __name__ == "__main__":
    unittest.main()
