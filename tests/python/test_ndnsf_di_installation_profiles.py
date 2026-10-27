from __future__ import annotations

import subprocess
import sys
import tempfile
import os
from pathlib import Path
import shutil
import unittest
import zipfile
import configparser
import textwrap
from email.parser import BytesParser


ROOT=Path(__file__).resolve().parents[2]
PROFILES=ROOT/"NDNSF-DistributedInference/packaging/python"


class InstallationProfilesTest(unittest.TestCase):
    def test_owner_wheels_build_and_records_do_not_collide(self):
        names=("core","sdk","app","planner","adapters/onnx","adapters/qwen","adapters/llama","ops","compat")
        owned={}; wheels={}; console_owners=[]
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
            shutil.copyfile(ROOT / 'NDNSF-DistributedInference/ndnsf_di_launcher.py',
                            source_root / 'ndnsf_di_launcher.py')
            profiles = source_root / "packaging/python"
            allocation_helper = ROOT / "packaging/ndnsf-di-container/lib/allocation_topology.py"
            staged_helper = temporary_root / "packaging/ndnsf-di-container/lib/allocation_topology.py"
            staged_helper.parent.mkdir(parents=True)
            shutil.copyfile(allocation_helper, staged_helper)
            for name in ('__init__.py', 'base.py', 'slurm_apptainer.py'):
                relative = Path('packaging/ndnsf-di-container/lib/adapters') / name
                target = temporary_root / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / relative, target)
            for name in ('run-allocation-topology.sh', 'configure-allocation-routes.sh',
                         'run-container.sh', 'stage-sif.sh', 'validate-local-sif-build-record.py'):
                relative = Path('Experiments/TigerCluster/adapters/slurm-apptainer/scripts') / name
                target = temporary_root / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / relative, target)
            process_helper = ROOT / "Experiments/TigerCluster/runtime/processes.py"
            staged_processes = temporary_root / "Experiments/TigerCluster/runtime/processes.py"
            staged_processes.parent.mkdir(parents=True)
            shutil.copyfile(process_helper, staged_processes)
            output = temporary_root / "wheels"
            output.mkdir()
            for name in names:
                # Match the installer's declared PEP 517 build dependencies.
                # Host setuptools can predate PEP 621 and ignore pyproject scripts.
                subprocess.run([sys.executable,"-m","pip","wheel","--no-deps",
                                str(profiles/name),"-w",output],check=True,capture_output=True,text=True)
            subprocess.run([sys.executable,"-m","pip","wheel","--no-deps","--no-build-isolation",
                            str(ROOT/"tests/fixtures/ndnsf-di-external-optimizer"),"-w",output],
                           check=True,capture_output=True,text=True)
            for wheel in Path(output).glob("*.whl"):
                with zipfile.ZipFile(wheel) as archive:
                    if wheel.name.startswith("ndnsf_di_ops-"):
                        self.assertEqual(
                            archive.read('ndnsf_di_launcher.py'),
                            (ROOT / 'NDNSF-DistributedInference/ndnsf_di_launcher.py').read_bytes())
                        self.assertEqual(
                            archive.read("ndnsf_distributed_inference/ops/allocation_topology.py"),
                            allocation_helper.read_bytes(),
                        )
                        self.assertEqual(
                            archive.read("ndnsf_distributed_inference/ops/processes.py"),
                            process_helper.read_bytes(),
                        )
                        for name in ('__init__.py', 'base.py', 'slurm_apptainer.py'):
                            self.assertEqual(archive.read(
                                'ndnsf_distributed_inference/ops/_container/lib/adapters/' + name),
                                (ROOT / 'packaging/ndnsf-di-container/lib/adapters' / name).read_bytes())
                        for name in ('run-allocation-topology.sh', 'configure-allocation-routes.sh',
                                     'run-container.sh', 'stage-sif.sh', 'validate-local-sif-build-record.py'):
                            relative = Path('adapters/slurm-apptainer/scripts') / name
                            self.assertEqual(archive.read(
                                'ndnsf_distributed_inference/ops/_container/' + relative.as_posix()),
                                (ROOT / 'Experiments/TigerCluster' / relative).read_bytes())
                    else:
                        self.assertNotIn('ndnsf_di_launcher.py', archive.namelist())
                    files={item for item in archive.namelist()
                           if item.startswith("ndnsf_distributed_inference/")}
                    for item in archive.namelist():
                        if item.endswith(".dist-info/METADATA"):
                            metadata = BytesParser().parsebytes(archive.read(item))
                            required = {
                                value.replace(" ", "")
                                for value in metadata.get_all("Requires-Dist", [])
                            }
                            expected = {
                                "ndnsf-di-core": {"cryptography>=2.8", "python-ndn"},
                                "ndnsf-di-app": {
                                    "ndnsf-di-core==0.111.0", "ndnsf-di-sdk==0.111.0",
                                    "ndnsf-di-planner==0.111.0", "cryptography>=2.8",
                                },
                            }.get(metadata["Name"], set())
                            self.assertLessEqual(expected, required, wheel.name)
                        if item.endswith(".dist-info/entry_points.txt"):
                            metadata = configparser.ConfigParser()
                            metadata.read_string(archive.read(item).decode("utf-8"))
                            if metadata.has_option("console_scripts", "ndnsf-di"):
                                console_owners.append(wheel.name)
                wheels[wheel.name]=files
            self.assertEqual(len(console_owners), 1, console_owners)
            self.assertTrue(console_owners[0].startswith("ndnsf_di_ops-"))
            self.assertEqual(len(wheels),10)
            items=list(wheels.items())
            for index,(left_name,left) in enumerate(items):
                for right_name,right in items[index+1:]:
                    collision=left & right
                    self.assertFalse(collision,f"{left_name}/{right_name}: {collision}")
            core=next(files for name,files in wheels.items() if name.startswith("ndnsf_di_core-"))
            self.assertTrue(core)
            core_extras = {path for path in core
                           if not path.startswith("ndnsf_distributed_inference/core/")}
            self.assertEqual(core_extras, {"ndnsf_distributed_inference/naming.py"})
            sdk=next(files for name,files in wheels.items() if name.startswith("ndnsf_di_sdk-"))
            self.assertIn("ndnsf_distributed_inference/conversation.py", sdk)
            self.assertIn("ndnsf_distributed_inference/adapters/__init__.py", sdk)
            self.assertIn("ndnsf_distributed_inference/adapters/base.py", sdk)
            self.assertIn("ndnsf_distributed_inference/adapters/builtin.py", sdk)
            app=next(files for name,files in wheels.items() if name.startswith("ndnsf_di_app-"))
            self.assertIn("ndnsf_distributed_inference/retry.py", app)
            self.assertIn("ndnsf_distributed_inference/runtime_v1.py", app)

            # Only management wheels are installed here. Third-party/native
            # dependencies come from the declared host runtime, never DI stubs.
            native_origin = subprocess.check_output(
                [sys.executable, "-I", "-c", "import importlib.util; "
                 "print(importlib.util.find_spec('ndnsf').origin)"], text=True).strip()
            native_init = Path(native_origin).resolve(strict=True)
            self.assertNotIn(ROOT, native_init.parents,
                             "management-wheel native dependency must be installed")
            # A system-site venv can prepend legacy sysconfig directories that
            # the host interpreter itself does not search. Load its declared
            # real Core, preserving the venv's independent DI wheel namespace.
            native_import = textwrap.dedent(f"""
                import importlib.machinery, importlib.util, sys
                from pathlib import Path
                spec = importlib.machinery.PathFinder.find_spec(
                    'ndnsf', [{str(native_init.parent.parent)!r}])
                assert spec is not None and Path(spec.origin).resolve() == Path({str(native_init)!r})
                native = importlib.util.module_from_spec(spec)
                sys.modules['ndnsf'] = native
                spec.loader.exec_module(native)
                import ndnsf._ndnsf
                assert Path(ndnsf.__file__).resolve() == Path({str(native_init)!r})
                assert Path(ndnsf._ndnsf.__file__).resolve().parent == Path({str(native_init.parent)!r})
            """)
            with tempfile.TemporaryDirectory() as environment:
                subprocess.run([sys.executable, "-m", "venv", "--system-site-packages",
                                environment], check=True)
                python = str(Path(environment) / "bin/python")
                clean_env = dict(os.environ)
                clean_env.pop("PYTHONPATH", None)
                clean_env["PYTHONNOUSERSITE"] = "1"
                clean_env["PYTHONUSERBASE"] = str(Path(environment) / "user-base")
                prefixes = ("ndnsf_di_core-", "ndnsf_di_sdk-", "ndnsf_di_app-",
                            "ndnsf_di_planner-", "ndnsf_di_ops-", "ndnsf_distributed_inference-")
                minimal = [str(path) for path in output.glob("*.whl")
                           if path.name.startswith(prefixes)]
                self.assertEqual(len(minimal), 6)
                subprocess.run([python, "-m", "pip", "install", "--no-deps",
                                "--ignore-installed", *minimal], check=True,
                               capture_output=True, text=True, env=clean_env)
                management_probe = textwrap.dedent("""
                    import importlib.abc, sys
                    from pathlib import Path

                    class RejectRuntime(importlib.abc.MetaPathFinder):
                        def find_spec(self, fullname, path=None, target=None):
                            if (fullname.startswith('ndnsf_distributed_inference.app_sdk') or
                                    fullname == 'ndnsf' or fullname.startswith('ndnsf.') or
                                    fullname == '_ndnsf'):
                                raise AssertionError('unexpected runtime import: ' + fullname)

                    sys.meta_path.insert(0, RejectRuntime())
                    import ndnsf_distributed_inference.ops.cli as cli
                    assert Path(sys.prefix) in Path(cli.__file__).resolve().parents
                    cli.main(['--help'])
                """)
                checked = subprocess.run(
                    [python, "-I", "-c", management_probe], cwd=temporary_root,
                    capture_output=True, text=True, env=clean_env,
                )
                self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
                # A legacy user .pth can prepend a source package even ahead
                # of this venv's installed wheels. The actual entry must
                # restart without it before importing DI.
                stale = temporary_root / 'legacy-source/ndnsf_distributed_inference'
                stale.mkdir(parents=True)
                (stale / '__init__.py').write_text("raise RuntimeError('WRONG_USER_SITE_DI')\n")
                user_site = Path(clean_env['PYTHONUSERBASE']) / 'lib' / (
                    'python%d.%d' % sys.version_info[:2]) / 'site-packages'
                user_site.mkdir(parents=True)
                (user_site / 'legacy-di.pth').write_text(
                    'import sys; sys.path.insert(0, ' + repr(str(stale.parent)) + ')\n')
                legacy_env = dict(clean_env)
                legacy_env.pop('PYTHONNOUSERSITE')
                control = subprocess.run([python, '-c', 'import ndnsf_distributed_inference'],
                                         capture_output=True, text=True, env=legacy_env,
                                         cwd=temporary_root)
                self.assertNotEqual(control.returncode, 0)
                self.assertIn('WRONG_USER_SITE_DI', control.stderr)
                console = Path(environment) / 'bin/ndnsf-di'
                checked = subprocess.run([str(console), 'cluster', '--help'],
                                         capture_output=True, text=True, env=legacy_env,
                                         cwd=temporary_root)
                self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
                self.assertIn('check', checked.stdout)
                self.assertNotIn('WRONG_USER_SITE_DI', checked.stderr)
                probe = native_import + textwrap.dedent("""
                    import contextlib, importlib.util, io, sys
                    from pathlib import Path
                    import ndnsf._ndnsf
                    import ndnsf_distributed_inference.adapters as adapters
                    for model in ('onnx', 'qwen', 'llama'):
                        assert importlib.util.find_spec(adapters.__name__ + '.' + model) is None
                    from ndnsf_distributed_inference.ops.cli import main
                    with contextlib.redirect_stdout(io.StringIO()):
                        try:
                            main(['--help'])
                        except SystemExit as error:
                            assert error.code == 0
                        else:
                            raise AssertionError('help did not exit')
                    assert not any(n.startswith(adapters.__name__ + '.' + model)
                                   for n in sys.modules for model in ('onnx', 'qwen', 'yolo'))
                    outside = [(n, m.__file__) for n, m in sys.modules.items()
                               if n.startswith('ndnsf_distributed_inference')
                               and getattr(m, '__file__', None)
                               and not str(Path(m.__file__).resolve()).startswith(sys.prefix + '/')]
                    assert not outside, outside
                """)
                checked = subprocess.run([python, "-c", probe], cwd=temporary_root,
                                         capture_output=True, text=True, env=clean_env)
                self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
                subprocess.run([python, "-m", "pip", "uninstall", "-y",
                                "ndnsf-distributed-inference"], check=True,
                               capture_output=True, text=True, env=clean_env)
                console = Path(environment) / "bin/ndnsf-di"
                self.assertTrue(console.is_file(), "compat uninstall removed the ops-owned CLI")
                # Without compat this is a namespace package. Restrict only
                # that namespace to this venv so a regular host DI package
                # cannot shadow it; third-party/native dependencies stay real.
                isolated_namespace = textwrap.dedent("""
                    import importlib.machinery, sys
                    from pathlib import Path
                    class InstalledDIOnly:
                        @staticmethod
                        def find_spec(name, path=None, target=None):
                            if name == 'ndnsf_distributed_inference':
                                site = str(Path(sys.executable).parent.parent / 'lib' /
                                           ('python%d.%d' % sys.version_info[:2]) / 'site-packages')
                                return importlib.machinery.PathFinder.find_spec(name, [site])
                    sys.meta_path.insert(0, InstalledDIOnly)
                """)
                console_probe = textwrap.dedent("""
                    import runpy
                    sys.argv = [sys.argv[1], '--help']
                    runpy.run_path(sys.argv[0], run_name='__main__')
                """)
                checked = subprocess.run([python, "-s", "-c", isolated_namespace + probe + console_probe,
                                          str(console)], cwd=temporary_root,
                                         capture_output=True, text=True, env=clean_env)
                self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

            with tempfile.TemporaryDirectory() as environment:
                # The profile wheels are installed without dependency resolution.
                # Reuse the test host's declared third-party dependencies offline;
                # verify below that every DI module still comes from these wheels.
                subprocess.run([sys.executable, "-m", "venv", "--system-site-packages", environment], check=True)
                python = str(Path(environment) / "bin/python")
                clean_env = dict(os.environ)
                clean_env.pop("PYTHONPATH", None)
                clean_env["PYTHONNOUSERSITE"] = "1"
                clean_env["PYTHONUSERBASE"] = str(Path(environment) / "user-base")
                wheel_paths = [str(path) for path in Path(output).glob("*.whl")]
                subprocess.run(
                    [python, "-m", "pip", "install", "--no-deps", "--ignore-installed", *wheel_paths],
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
                     "import ndnsf_distributed_inference.ops; "
                     "from pathlib import Path; "
                     "outside = [(n, m.__file__) for n, m in sys.modules.items() "
                     "if n.startswith('ndnsf_distributed_inference') and getattr(m, '__file__', None) "
                     "and not str(Path(m.__file__).resolve()).startswith(str(Path(sys.prefix).resolve()) + '/')]; "
                     "assert not outside, outside"],
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
                uninstalled = subprocess.run(
                    [python, "-m", "pip", "uninstall", "-y", *distributions],
                    check=True, capture_output=True, text=True, env=clean_env,
                )
                # System-site-packages deliberately exposes the host runtime.
                # Its packages may remain importable after the venv wheels are
                # removed; assert removal of the files this installation owned.
                site = (Path(environment) / "lib" /
                        f"python{sys.version_info.major}.{sys.version_info.minor}" /
                        "site-packages/ndnsf_distributed_inference")
                leftovers = sorted(str(path.relative_to(site))
                                   for path in site.rglob("*") if path.is_file())
                self.assertEqual(leftovers, [],
                                 "Remaining files: " + repr(leftovers) + "\n" +
                                 uninstalled.stdout + uninstalled.stderr)


if __name__=="__main__": unittest.main()
