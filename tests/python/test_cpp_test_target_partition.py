from __future__ import annotations

import ast
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def _program_keywords(name_value: str) -> dict[str, ast.expr]:
    tree = ast.parse((REPO_ROOT / "tests" / "wscript").read_text())
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        if not isinstance(node.func, ast.Attribute) or node.func.attr != "program":
            continue
        keywords = {item.arg: item.value for item in node.keywords if item.arg}
        name = keywords.get("name")
        if not isinstance(name, ast.Constant) or name.value != name_value:
            continue
        return keywords
    raise AssertionError(f"{name_value} target not found in tests/wscript")


def _unit_test_source_globs() -> list[str]:
        source = _program_keywords("unit-tests")["source"]
        globs: list[str] = []
        for child in ast.walk(source):
            if not isinstance(child, ast.Call):
                continue
            if not isinstance(child.func, ast.Attribute) or child.func.attr != "ant_glob":
                continue
            if child.args and isinstance(child.args[0], ast.Constant):
                globs.append(str(child.args[0].value))
        return globs


def _constant_strings(node: ast.expr) -> set[str]:
    return {
        str(child.value)
        for child in ast.walk(node)
        if isinstance(child, ast.Constant) and isinstance(child.value, str)
    }


def test_unit_target_does_not_recursively_include_integration_sources() -> None:
    globs = _unit_test_source_globs()
    assert "**/*.cpp" not in globs
    assert "unit-tests/**/*.cpp" in globs


def test_cpp_test_targets_disable_unsafe_gotpcrel_relaxation() -> None:
    for target in ("unit-tests", "integration-tests"):
        keywords = _program_keywords(target)
        assert "-Wl,--no-relax" in _constant_strings(keywords["linkflags"])
