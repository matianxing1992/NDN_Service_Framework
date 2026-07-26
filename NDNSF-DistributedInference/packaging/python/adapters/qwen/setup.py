from setuptools import find_packages, setup
setup(name="ndnsf-di-adapter-qwen", version="0.111.0", package_dir={"":"../../../.."}, packages=find_packages("../../../..", include=["ndnsf_distributed_inference.adapters.qwen*"]), install_requires=["ndnsf-di-sdk==0.111.0","ndnsf-di-planner==0.111.0"])
