from pathlib import Path
from setuptools import find_packages, setup
from setuptools.command.build_py import build_py as _build_py


class build_py(_build_py):
    def run(self):
        super().run()
        # The compatibility wheel owns the shared namespace facade.
        (Path(self.build_lib) / "ndnsf_distributed_inference/__init__.py").unlink(missing_ok=True)


setup(
    name="ndnsf-di-core", version="0.111.0", package_dir={"": "../../.."},
    packages=find_packages("../../..", include=["ndnsf_distributed_inference.core*"]),
    py_modules=["ndnsf_distributed_inference.naming"],
    cmdclass={"build_py": build_py},
    install_requires=["cryptography>=2.8", "python-ndn"],
)
