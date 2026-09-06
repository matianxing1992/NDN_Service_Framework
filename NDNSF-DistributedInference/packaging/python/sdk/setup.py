from pathlib import Path
from setuptools import find_packages, setup
from setuptools.command.build_py import build_py as _build_py


class build_py(_build_py):
    def run(self):
        super().run()
        # The compatibility wheel is the sole owner of the namespace facade.
        (Path(self.build_lib) / "ndnsf_distributed_inference/__init__.py").unlink(missing_ok=True)

setup(
    name="ndnsf-di-sdk",
    version="0.111.0",
    py_modules=["ndnsf_distributed_inference.conversation"],
    cmdclass={"build_py": build_py},
    package_dir={"": "../../.."},
    packages=find_packages(
        "../../..",
        include=[
            "ndnsf_distributed_inference.sdk*",
            "ndnsf_distributed_inference.adapters*",
        ],
        exclude=[
            "ndnsf_distributed_inference.adapters.onnx*",
            "ndnsf_distributed_inference.adapters.qwen*",
            "ndnsf_distributed_inference.adapters.llama*",
        ],
    ),
    install_requires=["ndnsf-di-core==0.111.0"],
)
