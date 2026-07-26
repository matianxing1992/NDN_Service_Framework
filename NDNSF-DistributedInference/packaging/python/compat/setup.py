from pathlib import Path
from shutil import copyfile
from setuptools import find_packages, setup
from setuptools.command.build_py import build_py as _build_py

class build_py(_build_py):
    def run(self):
        super().run()
        source = Path(__file__).resolve().parents[3] / "ndnsf_distributed_inference/compatibility/manifest.json"
        target = Path(self.build_lib) / "ndnsf_distributed_inference/compatibility/manifest.json"
        target.parent.mkdir(parents=True, exist_ok=True)
        copyfile(source, target)

setup(name="ndnsf-distributed-inference", version="0.111.0", package_dir={"":"src"}, packages=find_packages("src"), package_data={"ndnsf_distributed_inference.compatibility":["manifest.json"]}, cmdclass={"build_py":build_py}, install_requires=["ndnsf-di-core==0.111.0","ndnsf-di-sdk==0.111.0","ndnsf-di-app==0.111.0","ndnsf-di-planner==0.111.0","ndnsf-di-ops==0.111.0"], entry_points={"console_scripts":["ndnsf-di=ndnsf_distributed_inference.ops.cli:main"]})
