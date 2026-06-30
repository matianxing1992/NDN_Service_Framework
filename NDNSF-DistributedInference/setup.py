from setuptools import find_packages, setup


setup(
    name="ndnsf-distributed-inference",
    version="0.1.0",
    description="High-level distributed inference APIs built on NDNSF",
    packages=find_packages(),
    python_requires=">=3.8",
    entry_points={
        "console_scripts": [
            "ndnsf-di-policy=ndnsf_distributed_inference.policy:main",
            "ndnsf-di=ndnsf_distributed_inference.runtime_v1:main",
        ],
    },
)
