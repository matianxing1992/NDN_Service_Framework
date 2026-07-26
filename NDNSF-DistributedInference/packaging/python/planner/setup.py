from pathlib import Path
from setuptools import find_packages, setup
from setuptools.command.build_py import build_py as _build_py
class build_py(_build_py):
    def run(self):
        super().run()
        (Path(self.build_lib)/"ndnsf_distributed_inference/__init__.py").unlink(missing_ok=True)
setup(name="ndnsf-di-planner", version="0.111.0", package_dir={"":"../../.."}, packages=find_packages("../../..", include=["ndnsf_distributed_inference.planner*"]), py_modules=["ndnsf_distributed_inference.plan","ndnsf_distributed_inference.runtime_compatibility","ndnsf_distributed_inference.llm_stub_planner","ndnsf_distributed_inference.splitter"], cmdclass={"build_py":build_py}, install_requires=["ndnsf-di-core==0.111.0","ndnsf-di-sdk==0.111.0"])
