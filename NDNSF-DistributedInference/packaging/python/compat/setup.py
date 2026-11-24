from setuptools import find_packages, setup


setup(
    name="ndnsf-distributed-inference",
    version="0.111.0",
    package_dir={"": "src"},
    packages=find_packages("src", include=["ndnsf_distributed_inference"]),
    install_requires=[
        "ndnsf-di-core==0.111.0", "ndnsf-di-sdk==0.111.0",
        "ndnsf-di-app==0.111.0", "ndnsf-di-planner==0.111.0",
        "ndnsf-di-ops==0.111.0",
    ],
)
