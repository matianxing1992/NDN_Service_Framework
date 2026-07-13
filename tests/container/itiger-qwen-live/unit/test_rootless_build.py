from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "tools/ndnsf-di"))

from spec110_rootless_build import RootlessBuildError, render_rootless_build_job


class RootlessBuildRenderTests(unittest.TestCase):
    def test_render_is_cpu_only_checksum_bound_and_never_submits(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source"
            project = root / "project"
            output = project / "campaigns/spec110/rendered/probe.sbatch"
            source.mkdir()
            record = render_rootless_build_job(
                source_root=source,
                project_root=project,
                output_path=output,
                release_id="spec110-rootless-probe-001",
                allow_test_root=True,
            )
            text = output.read_text(encoding="utf-8")
            self.assertEqual(record["status"], "RENDERED_NOT_SUBMITTED")
            self.assertTrue(record["diagnosticOnly"])
            self.assertIsNone(record["resources"]["gres"])
            self.assertNotIn("--gres", text)
            self.assertNotIn("sbatch ", text)
            self.assertIn("--mode diagnostic", text)
            self.assertIn("rootless-build.sh", text)
            self.assertEqual(
                record["scriptSha256"],
                "sha256:" + hashlib.sha256(output.read_bytes()).hexdigest(),
            )
            persisted = json.loads(output.with_suffix(".sbatch.render.json").read_text())
            self.assertEqual(persisted, record)

    def test_render_rejects_overwrite_and_mutable_base(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source"
            project = root / "project"
            output = project / "campaigns/spec110/rendered/probe.sbatch"
            source.mkdir()
            kwargs = dict(
                source_root=source,
                project_root=project,
                output_path=output,
                release_id="probe-001",
                allow_test_root=True,
            )
            render_rootless_build_job(**kwargs)
            with self.assertRaisesRegex(RootlessBuildError, "RENDER_EXISTS"):
                render_rootless_build_job(**kwargs)
            with self.assertRaisesRegex(RootlessBuildError, "PROBE_BASE_NOT_PINNED"):
                render_rootless_build_job(
                    **{**kwargs, "output_path": output.parent / "other.sbatch", "probe_base": "alpine:latest"}
                )
            with self.assertRaisesRegex(RootlessBuildError, "OUTPUT_INVALID"):
                render_rootless_build_job(
                    **{**kwargs, "output_path": Path(str(output) + ";touch-pwned")}
                )


if __name__ == "__main__":
    unittest.main()
