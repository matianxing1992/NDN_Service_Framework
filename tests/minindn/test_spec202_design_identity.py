"""Progress may change; qualification requirements may not silently change."""

import tempfile
import unittest
from pathlib import Path

import spec193_preflight as preflight


ROOT = Path(__file__).resolve().parents[2]
FEATURE = ROOT / "specs/202-distributedrepo-convergence"


class DesignIdentityTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)

    def digest(self, name, text):
        path = Path(self.scratch.name) / name
        path.write_text(text)
        return preflight.design_acceptance_digest(path)

    def test_task_progress_and_checkboxes_preserve_acceptance(self):
        original = (FEATURE / "tasks.md").read_text()
        expected = self.digest("tasks.md", original)
        changed = original.replace("| T009 | PARTIAL |", "| T009 | DONE |")
        changed = changed.replace("2026-10-08", "2026-10-09")
        changed = changed.replace("- [x] T001", "- [ ] T001")
        changed = changed.replace("- [ ] T010", "- [x] T010")
        self.assertNotEqual(original, changed)
        self.assertEqual(expected, self.digest("tasks.md", changed))

    def test_task_requirements_dependencies_and_bindings_change_acceptance(self):
        original = (FEATURE / "tasks.md").read_text()
        expected = self.digest("tasks.md", original)
        for old, new in (("59case及网络子场景两轮", "59case及网络子场景一轮"),
                         ("T002–08→T009→T010", "T002–08→T010→T009"),
                         ("DB002 READY_FOR_IMPLEMENTATION", "DB002 BLOCKED")):
            with self.subTest(old=old):
                self.assertIn(old, original)
                self.assertNotEqual(expected, self.digest("tasks.md", original.replace(old, new)))

    def test_matrix_status_changes_preserve_oracles(self):
        original = (FEATURE / "validation-matrix.md").read_text()
        expected = self.digest("validation-matrix.md", original)
        changed = original.replace("**Status**: NOT_RUN", "**Status**: PASS")
        changed = changed.replace(" | NOT_RUN | NOT_RUN |", " | PASS | FAIL |")
        changed = changed.replace(" | NOT_RUN |\n", " | PASS |\n")
        self.assertNotEqual(original, changed)
        self.assertEqual(expected, self.digest("validation-matrix.md", changed))
        for old, new in (("完整 SHA-256", "部分 SHA-256"), ("T002 |", "T003 |")):
            with self.subTest(old=old):
                self.assertIn(old, original)
                self.assertNotEqual(expected, self.digest(
                    "validation-matrix.md", original.replace(old, new)))

    def test_missing_markers_tasks_cases_and_wrong_headers_reject(self):
        for name, old, new in (
                ("tasks.md", "<!-- SPEC202 PROGRESS END -->", ""),
                ("tasks.md", "- [ ] T010", "- [ ] T009"),
                ("validation-matrix.md", "<!-- SPEC202 QUALIFICATION STATUS START -->", ""),
                ("validation-matrix.md", "Round 1 | Round 2", "Oracle | Round 2"),
                ("validation-matrix.md", "| A01 |", "| A02 |")):
            with self.subTest(name=name, old=old):
                original = (FEATURE / name).read_text()
                self.assertIn(old, original)
                with self.assertRaises(preflight.PreflightError) as error:
                    self.digest(name, original.replace(old, new))
                self.assertEqual(error.exception.failure_class, "MALFORMED_DESIGN")
        with self.assertRaises(preflight.PreflightError) as error:
            preflight.design_acceptance_digest(Path(self.scratch.name) / "missing.md")
        self.assertEqual(error.exception.failure_class, "MISSING_DESIGN")


if __name__ == "__main__":
    unittest.main()
