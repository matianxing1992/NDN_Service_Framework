from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "tools/ndnsf-di"))

from spec110_rootless_build import RootlessBuildError, render_rootless_build_job


class RootlessBuildRenderTests(unittest.TestCase):
    def prepare_source(self, source: Path) -> None:
        target = source / "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts"
        target.mkdir(parents=True)
        owner = REPO / "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts"
        for name in ("rootless-build.sh", "inspect-oci-archive.py"):
            shutil.copy2(owner / name, target / name)

    def test_render_is_cpu_only_checksum_bound_and_never_submits(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source"
            project = root / "project"
            output = project / "campaigns/spec110/rendered/probe.sbatch"
            self.prepare_source(source)
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
            self.assertIn("--builder-mode auto", text)
            self.assertIn("--foundation-image docker.io/library/ubuntu@sha256:", text)
            self.assertIn(
                "--gpu-build-base docker.io/nvidia/cuda@sha256:f18cf1a9ac2842e59f13b0d0729594da8cbd68cadd2379308cdd98c0374dbd80",
                text,
            )
            self.assertIn(
                "--gpu-runtime-base docker.io/nvidia/cuda@sha256:a6a8417cb56c9a5d30c4d8c78ad18bc9b75ffe4453fe1c04b3149b3741518b06",
                text,
            )
            self.assertIn("quay.io/buildah/stable@sha256:", text)
            self.assertEqual(record["builder"]["requestedMode"], "auto")
            self.assertRegex(record["builder"]["ociDigest"], r"^sha256:[a-f0-9]{64}$")
            asset_root = output.with_suffix(".sbatch.assets")
            self.assertIn(str(asset_root / "rootless-build.sh"), text)
            self.assertEqual(set(record["assets"]), {"rootless-build.sh", "inspect-oci-archive.py"})
            for asset in record["assets"].values():
                path = Path(asset["path"])
                self.assertTrue(path.is_file())
                self.assertEqual(asset["sha256"], "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest())
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
            self.prepare_source(source)
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
            with self.assertRaisesRegex(RootlessBuildError, "BUILDER_OCI_NOT_PINNED"):
                render_rootless_build_job(
                    **{**kwargs, "output_path": output.parent / "builder.sbatch", "builder_oci": "quay.io/buildah/stable:latest"}
                )

    def test_apptainer_sif_backend_is_explicitly_rendered(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source"
            project = root / "project"
            output = project / "campaigns/spec110/rendered/fallback.sbatch"
            self.prepare_source(source)
            record = render_rootless_build_job(
                source_root=source,
                project_root=project,
                output_path=output,
                release_id="fallback-001",
                builder_mode="apptainer-sif",
                allow_test_root=True,
            )
            self.assertEqual(record["builder"]["requestedMode"], "apptainer-sif")
            self.assertIn("--builder-mode apptainer-sif", output.read_text())


if __name__ == "__main__":
    unittest.main()
