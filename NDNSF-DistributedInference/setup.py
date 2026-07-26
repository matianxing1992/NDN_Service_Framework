from setuptools import find_packages, setup


setup(
    name="ndnsf-distributed-inference",
    version="0.111.0",
    description="Bounded compatibility aggregate; install owner profiles for new deployments",
    packages=find_packages(include=[
        "ndnsf_distributed_inference",
        "ndnsf_distributed_inference.compatibility*",
    ]),
    python_requires=">=3.8",
    install_requires=[
        "ndnsf-di-core==0.111.0", "ndnsf-di-sdk==0.111.0",
        "ndnsf-di-app==0.111.0", "ndnsf-di-planner==0.111.0",
        "ndnsf-di-ops==0.111.0",
    ],
    entry_points={
        "console_scripts": [
            "ndnsf-di-policy=ndnsf_distributed_inference.policy:main",
            "ndnsf-di=ndnsf_distributed_inference.ops.cli:main",
        ],
    },
)
