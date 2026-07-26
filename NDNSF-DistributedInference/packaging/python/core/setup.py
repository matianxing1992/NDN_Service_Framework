from setuptools import find_packages, setup
setup(name="ndnsf-di-core", version="0.111.0", package_dir={"":"../../.."}, packages=find_packages("../../..", include=["ndnsf_distributed_inference.core*"]), install_requires=["cryptography>=2.8"])
