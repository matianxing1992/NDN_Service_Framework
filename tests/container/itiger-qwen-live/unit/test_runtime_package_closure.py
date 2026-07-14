from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[4]
SCRIPT = (
    REPO
    / "packaging/ndnsf-di-container/oci/scripts/derive-runtime-packages.py"
)


def load_runtime_package_module():
    spec = importlib.util.spec_from_file_location("derive_runtime_packages", SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RuntimePackageClosureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if shutil.which("gcc") is None or shutil.which("ldd") is None:
            raise RuntimeError("gcc and ldd are required for the real-ELF closure test")
        cls.module = load_runtime_package_module()

    def build_sibling_bundle(self, root: Path) -> tuple[Path, Path]:
        bundle = root / "pillow.libs"
        bundle.mkdir()
        dependency_source = root / "dependency.c"
        consumer_source = root / "consumer.c"
        dependency_source.write_text(
            "int spec110_sibling_value(void) { return 110; }\n",
            encoding="utf-8",
        )
        consumer_source.write_text(
            "extern int spec110_sibling_value(void);\n"
            "int spec110_consumer_value(void) { return spec110_sibling_value(); }\n",
            encoding="utf-8",
        )
        dependency = bundle / "libspec110_sibling.so.1"
        subprocess.run(
            [
                "gcc",
                "-fPIC",
                "-shared",
                str(dependency_source),
                "-Wl,-soname,libspec110_sibling.so.1",
                "-o",
                str(dependency),
            ],
            check=True,
            text=True,
            capture_output=True,
        )
        (bundle / "libspec110_sibling.so").symlink_to(dependency.name)
        consumer = bundle / "libspec110_consumer.so"
        subprocess.run(
            [
                "gcc",
                "-fPIC",
                "-shared",
                str(consumer_source),
                f"-L{bundle}",
                "-Wl,--no-as-needed",
                "-lspec110_sibling",
                "-o",
                str(consumer),
            ],
            check=True,
            text=True,
            capture_output=True,
        )
        return consumer, dependency

    def test_sibling_vendored_dso_is_part_of_runtime_closure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            consumer, dependency = self.build_sibling_bundle(Path(directory))
            clean_environment = dict(os.environ)
            clean_environment.pop("LD_LIBRARY_PATH", None)
            direct = subprocess.run(
                ["ldd", str(consumer)],
                env=clean_environment,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertIn("libspec110_sibling.so.1 => not found", direct.stdout)

            linked = self.module.linked_paths(consumer)

            self.assertIn(dependency.resolve(), linked)

    def test_missing_sibling_vendored_dso_still_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            consumer, dependency = self.build_sibling_bundle(Path(directory))
            dependency.unlink()

            with self.assertRaisesRegex(
                RuntimeError,
                rf"^RUNTIME_LIBRARY_MISSING:{consumer}$",
            ):
                self.module.linked_paths(consumer)

    def test_host_driver_libcuda_is_the_only_allowed_missing_dso(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            driver_source = root / "driver.c"
            consumer_source = root / "consumer.c"
            driver_source.write_text(
                "int spec110_cuda_driver_value(void) { return 110; }\n",
                encoding="utf-8",
            )
            consumer_source.write_text(
                "extern int spec110_cuda_driver_value(void);\n"
                "int main(void) { return spec110_cuda_driver_value() == 110 ? 0 : 1; }\n",
                encoding="utf-8",
            )
            driver = root / "libcuda.so.1"
            subprocess.run(
                [
                    "gcc",
                    "-fPIC",
                    "-shared",
                    str(driver_source),
                    "-Wl,-soname,libcuda.so.1",
                    "-o",
                    str(driver),
                ],
                check=True,
                text=True,
                capture_output=True,
            )
            (root / "libcuda.so").symlink_to(driver.name)
            consumer = root / "driver-consumer"
            subprocess.run(
                [
                    "gcc",
                    str(consumer_source),
                    f"-L{root}",
                    "-Wl,--no-as-needed",
                    "-lcuda",
                    "-o",
                    str(consumer),
                ],
                check=True,
                text=True,
                capture_output=True,
            )
            driver.unlink()
            (root / "libcuda.so").unlink()

            direct = subprocess.run(
                ["ldd", str(consumer)],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertIn("libcuda.so.1 => not found", direct.stdout)

            self.module.linked_paths(consumer)


if __name__ == "__main__":
    unittest.main()
