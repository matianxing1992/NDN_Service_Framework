from setuptools import find_packages, setup

setup(
    name="ndnsf-di-sdk",
    version="0.111.0",
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
