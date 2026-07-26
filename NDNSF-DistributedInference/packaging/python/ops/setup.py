from setuptools import find_packages, setup
setup(name="ndnsf-di-ops", version="0.111.0", package_dir={"":"../../.."}, packages=find_packages("../../..", include=["ndnsf_distributed_inference.ops*"]), install_requires=["ndnsf-di-app==0.111.0"], entry_points={"console_scripts":["ndnsf-di=ndnsf_distributed_inference.ops.cli:main"]})
