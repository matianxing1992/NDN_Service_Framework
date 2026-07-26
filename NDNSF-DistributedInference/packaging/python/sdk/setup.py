from setuptools import find_packages, setup
setup(name="ndnsf-di-sdk", version="0.111.0", package_dir={"":"../../.."}, packages=find_packages("../../..", include=["ndnsf_distributed_inference.sdk*"]), install_requires=["ndnsf-di-core==0.111.0"])
