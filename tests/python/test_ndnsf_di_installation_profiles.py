from __future__ import annotations

import subprocess
import sys
import tempfile
import os
from pathlib import Path
import shutil
import unittest
import zipfile


ROOT=Path(__file__).resolve().parents[2]
PROFILES=ROOT/"NDNSF-DistributedInference/packaging/python"


class InstallationProfilesTest(unittest.TestCase):
    def test_owner_wheels_build_and_records_do_not_collide(self):
        names=("core","sdk","app","planner","adapters/onnx","adapters/qwen","adapters/llama","ops","compat")
        owned={}; wheels={}
        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            source_root = temporary_root / "source"
            shutil.copytree(
                ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference",
                source_root / "ndnsf_distributed_inference",
                ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
            )
            shutil.copytree(
                PROFILES,
                source_root / "packaging/python",
                ignore=shutil.ignore_patterns(
                    "build", "dist", "*.egg-info", "__pycache__", "*.pyc"),
            )
            profiles = source_root / "packaging/python"
            output = temporary_root / "wheels"
            output.mkdir()
            for name in names:
                subprocess.run([sys.executable,"-m","pip","wheel","--no-deps","--no-build-isolation",
                                str(profiles/name),"-w",output],check=True,capture_output=True,text=True)
            subprocess.run([sys.executable,"-m","pip","wheel","--no-deps","--no-build-isolation",
                            str(ROOT/"tests/fixtures/ndnsf-di-external-optimizer"),"-w",output],
                           check=True,capture_output=True,text=True)
            for wheel in Path(output).glob("*.whl"):
                with zipfile.ZipFile(wheel) as archive:
                    files={item for item in archive.namelist()
                           if item.startswith("ndnsf_distributed_inference/")}
                wheels[wheel.name]=files
            self.assertEqual(len(wheels),10)
            items=list(wheels.items())
            for index,(left_name,left) in enumerate(items):
                for right_name,right in items[index+1:]:
                    collision=left & right
                    self.assertFalse(collision,f"{left_name}/{right_name}: {collision}")
            core=next(files for name,files in wheels.items() if name.startswith("ndnsf_di_core-"))
            self.assertTrue(core)
            self.assertTrue(all(path.startswith("ndnsf_distributed_inference/core/") for path in core))
            sdk=next(files for name,files in wheels.items() if name.startswith("ndnsf_di_sdk-"))
            self.assertIn("ndnsf_distributed_inference/adapters/__init__.py", sdk)
            self.assertIn("ndnsf_distributed_inference/adapters/base.py", sdk)
            self.assertIn("ndnsf_distributed_inference/adapters/builtin.py", sdk)

            with tempfile.TemporaryDirectory() as environment:
                subprocess.run([sys.executable, "-m", "venv", environment], check=True)
                python = str(Path(environment) / "bin/python")
                clean_env = dict(os.environ)
                clean_env.pop("PYTHONPATH", None)
                wheel_paths = [str(path) for path in Path(output).glob("*.whl")]
                subprocess.run(
                    [python, "-m", "pip", "install", "--no-deps", *wheel_paths],
                    check=True, capture_output=True, text=True, env=clean_env,
                )
                probe = subprocess.run(
                    [python, "-c", "import sys, types; "
                     "ndnsf = types.ModuleType('ndnsf'); "
                     "ndnsf.CollaborationDependency = type("
                     "'CollaborationDependency', (), {}); "
                     "ndnsf.CollaborationRole = type("
                     "'CollaborationRole', (), {}); "
                     "sys.modules['ndnsf'] = ndnsf; "
                     "import ndnsf_distributed_inference.core; "
                     "import ndnsf_distributed_inference.sdk; "
                     "from ndnsf_distributed_inference.adapters import "
                     "ApplicationInput, ModelFamilyAdapter; "
                     "import ndnsf_distributed_inference.adapters.onnx; "
                     "import ndnsf_distributed_inference.ops"],
                    check=False, capture_output=True, text=True, env=clean_env,
                )
                self.assertEqual(probe.returncode, 0, probe.stdout + probe.stderr)
                distributions = [
                    "ndnsf-di-core", "ndnsf-di-sdk", "ndnsf-di-app",
                    "ndnsf-di-planner", "ndnsf-di-adapter-onnx",
                    "ndnsf-di-adapter-qwen", "ndnsf-di-adapter-llama",
                    "ndnsf-di-ops", "ndnsf-distributed-inference",
                    "ndnsf-di-external-optimizer-fixture",
                ]
                subprocess.run(
                    [python, "-m", "pip", "uninstall", "-y", *distributions],
                    check=True, capture_output=True, text=True, env=clean_env,
                )
                absent = subprocess.run(
                    [python, "-c", "import ndnsf_distributed_inference.core"],
                    check=False, capture_output=True, text=True, env=clean_env,
                )
                self.assertNotEqual(absent.returncode, 0)


if __name__=="__main__": unittest.main()
