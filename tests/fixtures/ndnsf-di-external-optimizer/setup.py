from setuptools import find_packages, setup


setup(
    name="ndnsf-di-external-optimizer-fixture",
    version="1.0.0",
    description="Spec 111 standalone ten-policy optimizer fixture",
    package_dir={"": "src"},
    packages=find_packages("src"),
    entry_points={
        "ndnsf_di.optimizers": [
            "fixture=ndnsf_di_external_optimizer:create_suite",
        ],
    },
)
