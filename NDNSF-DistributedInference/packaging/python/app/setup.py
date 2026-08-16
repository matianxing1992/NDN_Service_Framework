from pathlib import Path
from setuptools import find_packages, setup
from setuptools.command.build_py import build_py as _build_py
class build_py(_build_py):
    def run(self):
        super().run()
        (Path(self.build_lib)/"ndnsf_distributed_inference/__init__.py").unlink(missing_ok=True)
setup(name="ndnsf-di-app", version="0.111.0", package_dir={"":"../../.."}, packages=find_packages("../../..", include=["ndnsf_distributed_inference.app_sdk*"]), py_modules=["ndnsf_distributed_inference.client","ndnsf_distributed_inference.deployment","ndnsf_distributed_inference.policy","ndnsf_distributed_inference.provider","ndnsf_distributed_inference.artifact_deployment","ndnsf_distributed_inference.repo_reference","ndnsf_distributed_inference.retry","ndnsf_distributed_inference.runtime_v1"], cmdclass={"build_py":build_py}, install_requires=["ndnsf-di-core==0.111.0","ndnsf-di-sdk==0.111.0","ndnsf-di-planner==0.111.0","cryptography>=2.8"])
